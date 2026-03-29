/*
 * Precompiled DXBC bytecode for softsplat compute shaders.
 *
 * Generated from softsplat.hlsl using:
 *   fxc /T cs_5_0 /E CSSoftsplatSplat /Fh softsplat_splat.h softsplat.hlsl
 *   fxc /T cs_5_0 /E CSSoftsplatNormalize /Fh softsplat_norm.h softsplat.hlsl
 *
 * TODO: Replace these placeholder arrays with actual compiled bytecode.
 * For now, the GMFSS backend will compile shaders at runtime using
 * D3DCompile() as a fallback if bytecode is empty.
 */

#pragma once

/* Placeholder: set to 0 length to trigger runtime compilation fallback */
static const unsigned char softsplat_splat_cs_bytecode[] = { 0 };
static const unsigned int softsplat_splat_cs_bytecode_len = 0;

static const unsigned char softsplat_normalize_cs_bytecode[] = { 0 };
static const unsigned int softsplat_normalize_cs_bytecode_len = 0;
