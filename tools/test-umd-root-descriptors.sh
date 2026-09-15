#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT
"${CC:-cc}" -std=c11 -O1 -g -Wall -Wextra -Werror -DNDEBUG \
  -fsanitize=address,undefined -fno-pie -no-pie \
  "$root/libs/vkd3d-umd/root_descriptor_test.c" -o "$tmp/root-descriptors"
"$tmp/root-descriptors"
