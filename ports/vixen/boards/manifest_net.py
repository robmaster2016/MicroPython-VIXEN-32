# SPDX-License-Identifier: MIT
# Copyright (c) 2025-2026 Robert S. (aka Robmaster)
# Frozen into the network build (make NET=1): the package installer and the
# HTTP client it uses, net (NETWORK.TXT -> network.LAN) and the SSH server.
require("mip")
module("net.py", base_path="$(PORT_DIR)/modules")
module("sshd.py", base_path="$(PORT_DIR)/modules")
