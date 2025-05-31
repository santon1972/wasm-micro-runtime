/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _AOT_COMPONENT_TYPES_H_
#define _AOT_COMPONENT_TYPES_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum AOTCanonTypeKind {
    AOT_CANON_TYPE_KIND_LIST,
    AOT_CANON_TYPE_KIND_RECORD,
    AOT_CANON_TYPE_KIND_VARIANT,
    AOT_CANON_TYPE_KIND_ENUM,
    AOT_CANON_TYPE_KIND_OPTION,
    AOT_CANON_TYPE_KIND_RESULT,
    AOT_CANON_TYPE_KIND_RESOURCE,
    AOT_CANON_TYPE_KIND_PRIMITIVE
} AOTCanonTypeKind;

typedef enum AOTCanonPrimValType {
    AOT_CANON_PRIM_VAL_TYPE_BOOL,
    AOT_CANON_PRIM_VAL_TYPE_S8,
    AOT_CANON_PRIM_VAL_TYPE_U8,
    AOT_CANON_PRIM_VAL_TYPE_S16,
    AOT_CANON_PRIM_VAL_TYPE_U16,
    AOT_CANON_PRIM_VAL_TYPE_S32,
    AOT_CANON_PRIM_VAL_TYPE_U32,
    AOT_CANON_PRIM_VAL_TYPE_S64,
    AOT_CANON_PRIM_VAL_TYPE_U64,
    AOT_CANON_PRIM_VAL_TYPE_F32,
    AOT_CANON_PRIM_VAL_TYPE_F64,
    AOT_CANON_PRIM_VAL_TYPE_CHAR,
    AOT_CANON_PRIM_VAL_TYPE_STRING
} AOTCanonPrimValType;

struct AOTCanonValType;

typedef struct AOTCanonListType {
    struct AOTCanonValType *elem_type;
} AOTCanonListType;

typedef struct AOTCanonRecordFieldType {
    char *name;
    struct AOTCanonValType *type;
} AOTCanonRecordFieldType;

typedef struct AOTCanonRecordType {
    uint32_t num_fields;
    AOTCanonRecordFieldType *fields;
} AOTCanonRecordType;

typedef struct AOTCanonVariantCaseType {
    char *name;
    struct AOTCanonValType *type; /* NULL if no payload */
} AOTCanonVariantCaseType;

typedef struct AOTCanonVariantType {
    uint32_t num_cases;
    AOTCanonVariantCaseType *cases;
} AOTCanonVariantType;

typedef struct AOTCanonEnumType {
    uint32_t num_cases;
    char **case_names;
} AOTCanonEnumType;

typedef struct AOTCanonOptionType {
    struct AOTCanonValType *inner_type;
} AOTCanonOptionType;

typedef struct AOTCanonResultType {
    struct AOTCanonValType *ok_type;  /* NULL if no ok type */
    struct AOTCanonValType *err_type; /* NULL if no err type */
} AOTCanonResultType;

typedef struct AOTCanonResourceType {
    uint32_t resource_id; /* Or index, depending on final design */
} AOTCanonResourceType;

typedef struct AOTCanonValType {
    AOTCanonTypeKind kind;
    union {
        AOTCanonListType list;
        AOTCanonRecordType record;
        AOTCanonVariantType variant;
        AOTCanonEnumType enum_type;
        AOTCanonOptionType option;
        AOTCanonResultType result;
        AOTCanonResourceType resource;
        AOTCanonPrimValType primitive;
    } u;
} AOTCanonValType;

#ifdef __cplusplus
} /* end of extern "C" */
#endif

#endif /* end of _AOT_COMPONENT_TYPES_H_ */
