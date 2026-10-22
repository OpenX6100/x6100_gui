/*
 * SPDX-License-Identifier: LGPL-2.1-or-later
 * Copyright (C) 2026 Hany El Imam - 1KO125 firmware for the Xiegu X6100
 */
#pragma once

/* Ensure the persistent SSTV directory tree exists and install the bundled
 * TX pictures/templates once per resource version. */
void sstv_resources_init(void);
