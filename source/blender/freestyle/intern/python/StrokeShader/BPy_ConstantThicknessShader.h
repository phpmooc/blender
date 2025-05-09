/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup freestyle
 */

#pragma once

#include "../BPy_StrokeShader.h"

///////////////////////////////////////////////////////////////////////////////////////////

extern PyTypeObject ConstantThicknessShader_Type;

#define BPy_ConstantThicknessShader_Check(v) \
  (PyObject_IsInstance((PyObject *)v, (PyObject *)&ConstantThicknessShader_Type))

/*---------------------------Python BPy_ConstantThicknessShader structure definition----------*/
typedef struct {
  BPy_StrokeShader py_ss;
} BPy_ConstantThicknessShader;

///////////////////////////////////////////////////////////////////////////////////////////
