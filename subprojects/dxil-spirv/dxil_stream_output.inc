/* SPDX-License-Identifier: MIT */
/* The scalar callback is opt-in; existing whole-variable remappers keep their signatures
 * and behavior. Native R0 signatures contain one row per semantic. Full, contiguous
 * captures can still decorate the original vector; only disjoint captures split it. */
unsigned Converter::Impl::remap_geometry_stream(unsigned stream)
{
	unsigned rasterized_stream;
	if (execution_model == spv::ExecutionModelGeometry && resource_mapping_iface &&
	    resource_mapping_iface->get_geometry_rasterization_stream(rasterized_stream) && rasterized_stream < 4)
	{
		if (stream == rasterized_stream) return 0;
		if (stream == 0) return rasterized_stream;
	}
	return stream;
}

DXIL::Semantic Converter::Impl::get_effective_output_semantic(const llvm::MDNode *output)
{
	unsigned rasterized_stream;
	if (execution_model == spv::ExecutionModelGeometry && resource_mapping_iface &&
	    resource_mapping_iface->get_geometry_rasterization_stream(rasterized_stream) &&
	    get_geometry_shader_stream_index(output) != rasterized_stream &&
	    !(execution_mode_meta.gs_stream_active_mask == 1 && rasterized_stream == ~0u))
		return DXIL::Semantic::User;
	return static_cast<DXIL::Semantic>(get_constant_metadata(output, 3));
}

bool Converter::Impl::prepare_stream_output_locations(const llvm::MDNode *outputs, const unsigned stream_offsets[4])
{
	// Preserve user-register linkage first. Builtins on non-rasterized streams
	// are ordinary outputs, but DXBC IR intentionally discards their registers;
	// place those in free locations after reserving all linked user variables.
	for (unsigned pass = 0; pass < 2; ++pass)
	for (unsigned i = 0; i < outputs->getNumOperands(); ++i)
	{
		auto *output = llvm::cast<llvm::MDNode>(outputs->getOperand(i));
		if (get_effective_output_semantic(output) != DXIL::Semantic::User)
			continue;
		bool ignored_builtin = static_cast<DXIL::Semantic>(get_constant_metadata(output, 3)) != DXIL::Semantic::User;
		if (ignored_builtin != bool(pass))
			continue;
		auto name = get_string_metadata(output, 1);
		unsigned index = output->getOperand(4) ? get_constant_metadata(llvm::cast<llvm::MDNode>(output->getOperand(4)), 0) : 0;
		unsigned row = get_constant_metadata(output, 8), col = get_constant_metadata(output, 9);
		unsigned rows = get_constant_metadata(output, 6), cols = get_constant_metadata(output, 7);
		unsigned stream = execution_model == spv::ExecutionModelGeometry ? get_geometry_shader_stream_index(output) : 0;
		VulkanStageIO location = {row + (stream < 4 ? stream_offsets[stream] : 0), col};
		if (ignored_builtin)
		{
			if (rows != 1 || !cols || cols > 4)
			{
				LOGE("Non-rasterized builtin arrays require preserved register metadata.\n");
				return false;
			}
			location = {32, 0};
			for (unsigned r = 0; r < 32 && location.location == 32; ++r)
				for (unsigned c = 0; c + cols <= 4; ++c)
					if (!(component_output_used[r] & (((1u << cols) - 1) << c)))
					{
						location = {r, c};
						break;
					}
		}
		else if (!resource_mapping_iface->remap_stage_output({name.c_str(), index, row, rows}, location))
			return false;
		if (!rows || rows > 32 || !cols || cols > 4 || location.location >= 32 ||
		    rows > 32 - location.location || location.component > 4 - cols)
		{
			LOGE("Stream output signature exceeds the native 32-register interface.\n");
			return false;
		}
		component_output_locations[get_constant_metadata(output, 0)] = location;
		for (unsigned r = 0; r < rows; ++r)
			component_output_used[location.location + r] |= ((1u << cols) - 1) << location.component;
	}
	return true;
}

bool Converter::Impl::remap_clip_cull_components(DXIL::Semantic semantic, unsigned stream,
                                                unsigned rows, VulkanStreamOutput *mappings)
{
	auto *metadata = bitcode_parser.get_module().getNamedMetadata("dxbc.clipCullOutputSignature");
	if (!metadata || !metadata->getNumOperands())
		return false;
	auto *entries = llvm::cast<llvm::MDNode>(metadata->getOperand(0));
	struct Component
	{
		const llvm::MDNode *entry = nullptr;
		unsigned component = 0;
	} components[8];
	for (unsigned i = 0; i < entries->getNumOperands(); ++i)
	{
		auto *entry = llvm::cast<llvm::MDNode>(entries->getOperand(i));
		if (get_constant_metadata(entry, 2) != stream ||
		    get_constant_metadata(entry, 3) != unsigned(semantic))
			continue;
		unsigned index = get_constant_metadata(entry, 1), mask = get_constant_metadata(entry, 4);
		if (index >= 2 || !mask || mask > 15)
			return false;
		unsigned first = 0;
		while (!(mask & (1u << first))) ++first;
		for (unsigned c = first; c < 4; ++c)
			if (mask & (1u << c))
			{
				auto &component = components[4 * index + c];
				if (component.entry) return false;
				component = {entry, c - first};
			}
	}
	// dxbc-spirv packs scalar array rows by set bits in semanticIndex*4+mask,
	// independently of declaration order and which subset the caller captures.
	unsigned row = 0;
	for (const auto &component : components)
		if (component.entry)
		{
			if (row >= rows) return false;
			auto name = get_string_metadata(component.entry, 0);
			if (!resource_mapping_iface->remap_stream_output_component({name.c_str(),
			        get_constant_metadata(component.entry, 1), stream, component.component}, mappings[row++]))
				return false;
		}
	return row == rows;
}

bool Converter::Impl::emit_stream_output_components(const llvm::MDNode *output,
                                                     VulkanStreamOutput &full_output, bool &replaced)
{
	auto &builder = spirv_module.get_builder();
	auto name = get_string_metadata(output, 1);
	unsigned index = output->getOperand(4) ? get_constant_metadata(llvm::cast<llvm::MDNode>(output->getOperand(4)), 0) : 0;
	unsigned rows = get_constant_metadata(output, 6), cols = get_constant_metadata(output, 7);
	unsigned id = get_constant_metadata(output, 0);
	unsigned stream = execution_model == spv::ExecutionModelGeometry ? get_geometry_shader_stream_index(output) : 0;
	auto semantic = get_effective_output_semantic(output);
	auto actual_type = normalize_component_type(static_cast<DXIL::ComponentType>(get_constant_metadata(output, 2)));
	auto effective_type = get_effective_input_output_type(actual_type);
	auto interpolation = static_cast<DXIL::InterpolationMode>(get_constant_metadata(output, 5));
	// DXBC lowering merges Clip/Cull signature vectors into scalar builtin
	// arrays. Original semantic/mask metadata resolves each compressed row;
	// the legacy IR-only path is limited to one vector without that metadata.
	const bool scalar_array = module_is_dxbc_spirv(bitcode_parser.get_module()) && cols == 1 && rows <= 8 &&
	        (semantic == DXIL::Semantic::ClipDistance || semantic == DXIL::Semantic::CullDistance);
	VulkanStreamOutput mappings[128] = {};
	if (!rows || rows > 32 || !cols || cols > 4 || stream >= 4)
		return false;
	const bool preserved_clip_cull = scalar_array &&
	        bitcode_parser.get_module().getNamedMetadata("dxbc.clipCullOutputSignature");
	if (scalar_array && !preserved_clip_cull && rows > 4)
		return false;
	if (preserved_clip_cull && !remap_clip_cull_components(semantic, stream, rows, mappings))
		return false;
	bool any = false, contiguous = true;
	for (unsigned r = 0; r < rows; ++r)
	{
		for (unsigned c = 0; c < cols; ++c)
		{
			auto &binding = mappings[r * cols + c];
			if (!preserved_clip_cull && !resource_mapping_iface->remap_stream_output_component({name.c_str(), index + (scalar_array ? 0 : r), stream, scalar_array ? r : c}, binding))
				return false;
			any |= binding.enable;
			contiguous &= binding.enable && binding.stride == mappings[0].stride &&
			              binding.buffer_index == mappings[0].buffer_index &&
			              binding.offset == mappings[0].offset + 4 * (r * cols + c);
		}
	}
	if (!any)
		return true;
	if (contiguous && semantic != DXIL::Semantic::ClipDistance && semantic != DXIL::Semantic::CullDistance &&
	    (semantic != DXIL::Semantic::Position || cols == 4))
	{
		full_output = mappings[0];
		return true;
	}
	if ((!scalar_array && rows != 1) || (actual_type != DXIL::ComponentType::F32 && actual_type != DXIL::ComponentType::I32 &&
	                  actual_type != DXIL::ComponentType::U32))
	{
		LOGE("Partial stream output requires a single 32-bit R0 signature row.\n");
		return false;
	}
	auto &components = component_outputs[id];
	components.scalar_array = scalar_array;
	replaced = components.replaces_original = semantic == DXIL::Semantic::User;
	for (unsigned c = 0; c < (scalar_array ? rows : cols); ++c)
	{
		if (!replaced && !mappings[c].enable)
			continue;
		VulkanStageIO location = {};
		if (replaced)
		{
			location = component_output_locations[id];
			location.component += c;
		}
		else
		{
			// Builtins keep the original vector for rasterization. Reuse genuinely
			// unoccupied components within the 32 native output registers for XFB.
			unsigned free_component = 0;
			while (free_component < 128 &&
			       (component_output_used[free_component / 4] & (1u << (free_component % 4))))
				++free_component;
			if (free_component == 128)
			{
				LOGE("No free interface component for partial builtin stream output.\n");
				return false;
			}
			location.location = free_component / 4;
			location.component = free_component % 4;
			component_output_used[location.location] |= 1u << location.component;
		}
		spv::Id variable = create_variable(spv::StorageClassOutput, get_type_id(effective_type, 1, 1),
		                                  (name + "_xfb_" + dxil_spv::to_string(c)).c_str());
		components.ids[c] = variable;
		builder.addDecoration(variable, spv::DecorationLocation, location.location);
		if (location.component)
			builder.addDecoration(variable, spv::DecorationComponent, location.component);
		emit_interpolation_decorations(variable, interpolation);
		unsigned physical_stream = remap_geometry_stream(stream);
		if (physical_stream)
		{
			builder.addCapability(spv::CapabilityGeometryStreams);
			builder.addDecoration(variable, spv::DecorationStream, physical_stream);
		}
		if (mappings[c].enable)
		{
			builder.addCapability(spv::CapabilityTransformFeedback);
			builder.addExecutionMode(spirv_module.get_entry_function(), spv::ExecutionModeXfb);
			builder.addDecoration(variable, spv::DecorationOffset, mappings[c].offset);
			builder.addDecoration(variable, spv::DecorationXfbStride, mappings[c].stride);
			builder.addDecoration(variable, spv::DecorationXfbBuffer, mappings[c].buffer_index);
		}
	}
	return true;
}
