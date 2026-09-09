/*
 * This file is part of mpv.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef MP_DLSSNR_MODEL_IDENTITY_H
#define MP_DLSSNR_MODEL_IDENTITY_H

#include <windows.h>

#include "gpu.h"

void dlssnr_model_identify(HANDLE file, struct dlssnr_gpu_info *info);

#endif
