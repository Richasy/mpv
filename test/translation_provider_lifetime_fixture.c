/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "player/whisper_translate_test.h"

__declspec(dllexport)
int translation_provider_lifetime_start(void);

__declspec(dllexport)
int translation_provider_lifetime_start(void)
{
    return whisper_translate_test_start_cleanup_service() ? 0 : 1;
}
