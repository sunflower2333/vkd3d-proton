# GPU source checkpoint, 2026-11-08

Commit current DX12 depth flag normalization, allocation alignment and submission lifetime fixes, runtime coverage, guarded hardware probes and x86/x64 packaging inputs. The original AIO64 depth initialization was validated in a bounded animated scene; the user also reports current AIO x86 DX12 works. Production allocator ownership and logical queue host controls pass. Retain unsupported cases and failed evidence; these results do not prove full DX12 conformance, continuous 165 Hz cadence or zero-copy performance.
