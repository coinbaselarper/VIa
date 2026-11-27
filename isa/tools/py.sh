#!/bin/bash
# python3 with capstone + pefile (uv-managed, isolated mode)
exec uv run --no-project --with capstone==5.0.9 --with pefile python3 -I "$@"
