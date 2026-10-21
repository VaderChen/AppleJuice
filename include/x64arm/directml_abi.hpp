#pragma once
// 自動產生：scripts/generate_directml_abi.py，DirectML.h（Microsoft.AI.DirectML 1.15.4 include/，Copyright Microsoft，MIT 授權）。
// 只含介面 IID、完整 vtable 順序與參數數量（不含 This）；以值回傳結構的方法含隱藏 RetVal 指標。請勿手動修改。
#include <array>
#include <cstdint>
#include <span>
#include <string_view>

namespace x64arm::dmlabi {
struct Method {std::string_view name; unsigned arguments;};
struct Interface {std::string_view name; std::array<std::uint8_t,16> iid; std::span<const Method> methods;};
// 運算子描述欄位：kind 0 輸入張量、1 輸出張量、2 屬性；type 依 DML_SCHEMA_FIELD_TYPE 順序；count 為陣列長度欄位索引（-1 表示非陣列）。
struct OperatorField {std::string_view name; unsigned kind; unsigned type; std::uint32_t offset; int count; bool optional;};
struct OperatorSchema {std::string_view name; std::uint32_t type; std::span<const OperatorField> fields; std::uint32_t size;};
inline constexpr Method IDMLObject_methods[]{{"QueryInterface",2},{"AddRef",0},{"Release",0},{"GetPrivateData",3},{"SetPrivateData",3},{"SetPrivateDataInterface",2},{"SetName",1}};
inline constexpr Method IDMLDevice_methods[]{{"QueryInterface",2},{"AddRef",0},{"Release",0},{"GetPrivateData",3},{"SetPrivateData",3},{"SetPrivateDataInterface",2},{"SetName",1},{"CheckFeatureSupport",5},{"CreateOperator",3},{"CompileOperator",4},{"CreateOperatorInitializer",4},{"CreateCommandRecorder",2},{"CreateBindingTable",3},{"Evict",2},{"MakeResident",2},{"GetDeviceRemovedReason",0},{"GetParentDevice",2}};
inline constexpr Method IDMLDeviceChild_methods[]{{"QueryInterface",2},{"AddRef",0},{"Release",0},{"GetPrivateData",3},{"SetPrivateData",3},{"SetPrivateDataInterface",2},{"SetName",1},{"GetDevice",2}};
inline constexpr Method IDMLPageable_methods[]{{"QueryInterface",2},{"AddRef",0},{"Release",0},{"GetPrivateData",3},{"SetPrivateData",3},{"SetPrivateDataInterface",2},{"SetName",1},{"GetDevice",2}};
inline constexpr Method IDMLOperator_methods[]{{"QueryInterface",2},{"AddRef",0},{"Release",0},{"GetPrivateData",3},{"SetPrivateData",3},{"SetPrivateDataInterface",2},{"SetName",1},{"GetDevice",2}};
inline constexpr Method IDMLDispatchable_methods[]{{"QueryInterface",2},{"AddRef",0},{"Release",0},{"GetPrivateData",3},{"SetPrivateData",3},{"SetPrivateDataInterface",2},{"SetName",1},{"GetDevice",2},{"GetBindingProperties",1}};
inline constexpr Method IDMLCompiledOperator_methods[]{{"QueryInterface",2},{"AddRef",0},{"Release",0},{"GetPrivateData",3},{"SetPrivateData",3},{"SetPrivateDataInterface",2},{"SetName",1},{"GetDevice",2},{"GetBindingProperties",1}};
inline constexpr Method IDMLOperatorInitializer_methods[]{{"QueryInterface",2},{"AddRef",0},{"Release",0},{"GetPrivateData",3},{"SetPrivateData",3},{"SetPrivateDataInterface",2},{"SetName",1},{"GetDevice",2},{"GetBindingProperties",1},{"Reset",2}};
inline constexpr Method IDMLBindingTable_methods[]{{"QueryInterface",2},{"AddRef",0},{"Release",0},{"GetPrivateData",3},{"SetPrivateData",3},{"SetPrivateDataInterface",2},{"SetName",1},{"GetDevice",2},{"BindInputs",2},{"BindOutputs",2},{"BindTemporaryResource",1},{"BindPersistentResource",1},{"Reset",1}};
inline constexpr Method IDMLCommandRecorder_methods[]{{"QueryInterface",2},{"AddRef",0},{"Release",0},{"GetPrivateData",3},{"SetPrivateData",3},{"SetPrivateDataInterface",2},{"SetName",1},{"GetDevice",2},{"RecordDispatch",3}};
inline constexpr Method IDMLDebugDevice_methods[]{{"QueryInterface",2},{"AddRef",0},{"Release",0},{"SetMuteDebugOutput",1}};
inline constexpr Method IDMLDevice1_methods[]{{"QueryInterface",2},{"AddRef",0},{"Release",0},{"GetPrivateData",3},{"SetPrivateData",3},{"SetPrivateDataInterface",2},{"SetName",1},{"CheckFeatureSupport",5},{"CreateOperator",3},{"CompileOperator",4},{"CreateOperatorInitializer",4},{"CreateCommandRecorder",2},{"CreateBindingTable",3},{"Evict",2},{"MakeResident",2},{"GetDeviceRemovedReason",0},{"GetParentDevice",2},{"CompileGraph",4}};
inline constexpr Interface interfaces[]{
    {"IDMLObject",{0xac,0x3a,0x26,0xc8,0x0c,0x9e,0x2d,0x4a,0x9b,0x8e,0x00,0x75,0x21,0xa3,0x31,0x7c},IDMLObject_methods},
    {"IDMLDevice",{0x37,0x64,0xbd,0x6d,0xfd,0x96,0x3f,0x42,0xa9,0x8c,0xae,0x5e,0x7c,0x2a,0x57,0x3f},IDMLDevice_methods},
    {"IDMLDeviceChild",{0x42,0x31,0xe8,0x27,0x65,0x81,0xe3,0x49,0x97,0x4e,0x2f,0xd6,0x6e,0x4c,0xb6,0x9d},IDMLDeviceChild_methods},
    {"IDMLPageable",{0x25,0x08,0xab,0xb1,0x42,0x45,0x4b,0x4a,0x86,0x17,0x6d,0xde,0x6e,0x8f,0x62,0x01},IDMLPageable_methods},
    {"IDMLOperator",{0x7a,0xae,0xca,0x26,0x81,0x30,0x33,0x46,0x95,0x81,0x22,0x6f,0xbe,0x57,0x69,0x5d},IDMLOperator_methods},
    {"IDMLDispatchable",{0xa8,0x21,0xb8,0xdc,0x39,0x10,0x1e,0x44,0x9f,0x1c,0xb1,0x75,0x9c,0x2f,0x3c,0xec},IDMLDispatchable_methods},
    {"IDMLCompiledOperator",{0x6a,0xe5,0x15,0x6b,0x5c,0xbf,0x02,0x49,0x92,0xd8,0xda,0x3a,0x65,0x0a,0xfe,0xa4},IDMLCompiledOperator_methods},
    {"IDMLOperatorInitializer",{0x13,0x11,0x7c,0x42,0x5c,0x43,0x9c,0x46,0x86,0x76,0x4d,0x5d,0xd0,0x72,0xf8,0x13},IDMLOperatorInitializer_methods},
    {"IDMLBindingTable",{0xdc,0x87,0xc6,0x29,0x74,0xde,0x3b,0x4e,0xab,0x00,0x11,0x68,0xf2,0xfc,0x3c,0xfc},IDMLBindingTable_methods},
    {"IDMLCommandRecorder",{0x76,0x7a,0x85,0xe6,0x3e,0x2e,0xdd,0x4f,0xbf,0xf4,0x5d,0x2b,0xa1,0x0f,0xb4,0x53},IDMLCommandRecorder_methods},
    {"IDMLDebugDevice",{0xc9,0x3a,0x6f,0x7d,0x4a,0x39,0xc3,0x4a,0x92,0xa7,0x39,0x0c,0xc5,0x7a,0x82,0x17},IDMLDebugDevice_methods},
    {"IDMLDevice1",{0x9a,0x4f,0x88,0xa0,0xbe,0xd2,0x55,0x43,0xaa,0x5d,0x59,0x01,0x28,0x1a,0xd1,0xd2},IDMLDevice1_methods},
};
// 列舉值（DirectML.h，最新目標版本）。
inline constexpr std::uint32_t DML_TENSOR_DATA_TYPE_UNKNOWN=0x0u;
inline constexpr std::uint32_t DML_TENSOR_DATA_TYPE_FLOAT32=0x1u;
inline constexpr std::uint32_t DML_TENSOR_DATA_TYPE_FLOAT16=0x2u;
inline constexpr std::uint32_t DML_TENSOR_DATA_TYPE_UINT32=0x3u;
inline constexpr std::uint32_t DML_TENSOR_DATA_TYPE_UINT16=0x4u;
inline constexpr std::uint32_t DML_TENSOR_DATA_TYPE_UINT8=0x5u;
inline constexpr std::uint32_t DML_TENSOR_DATA_TYPE_INT32=0x6u;
inline constexpr std::uint32_t DML_TENSOR_DATA_TYPE_INT16=0x7u;
inline constexpr std::uint32_t DML_TENSOR_DATA_TYPE_INT8=0x8u;
inline constexpr std::uint32_t DML_TENSOR_DATA_TYPE_FLOAT64=0x9u;
inline constexpr std::uint32_t DML_TENSOR_DATA_TYPE_UINT64=0xau;
inline constexpr std::uint32_t DML_TENSOR_DATA_TYPE_INT64=0xbu;
inline constexpr std::uint32_t DML_TENSOR_DATA_TYPE_UINT4=0xcu;
inline constexpr std::uint32_t DML_TENSOR_DATA_TYPE_INT4=0xdu;
inline constexpr std::uint32_t DML_TENSOR_TYPE_INVALID=0x0u;
inline constexpr std::uint32_t DML_TENSOR_TYPE_BUFFER=0x1u;
inline constexpr std::uint32_t DML_TENSOR_FLAG_NONE=0x0u;
inline constexpr std::uint32_t DML_TENSOR_FLAG_OWNED_BY_DML=0x1u;
inline constexpr std::uint32_t DML_OPERATOR_INVALID=0x0u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_IDENTITY=0x1u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_ABS=0x2u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_ACOS=0x3u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_ADD=0x4u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_ASIN=0x5u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_ATAN=0x6u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_CEIL=0x7u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_CLIP=0x8u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_COS=0x9u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_DIVIDE=0xau;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_EXP=0xbu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_FLOOR=0xcu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_LOG=0xdu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_LOGICAL_AND=0xeu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_LOGICAL_EQUALS=0xfu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_LOGICAL_GREATER_THAN=0x10u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_LOGICAL_LESS_THAN=0x11u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_LOGICAL_NOT=0x12u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_LOGICAL_OR=0x13u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_LOGICAL_XOR=0x14u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_MAX=0x15u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_MEAN=0x16u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_MIN=0x17u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_MULTIPLY=0x18u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_POW=0x19u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_CONSTANT_POW=0x1au;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_RECIP=0x1bu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_SIN=0x1cu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_SQRT=0x1du;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_SUBTRACT=0x1eu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_TAN=0x1fu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_THRESHOLD=0x20u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_QUANTIZE_LINEAR=0x21u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_DEQUANTIZE_LINEAR=0x22u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_ELU=0x23u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_HARDMAX=0x24u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_HARD_SIGMOID=0x25u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_IDENTITY=0x26u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_LEAKY_RELU=0x27u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_LINEAR=0x28u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_LOG_SOFTMAX=0x29u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_PARAMETERIZED_RELU=0x2au;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_PARAMETRIC_SOFTPLUS=0x2bu;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_RELU=0x2cu;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_SCALED_ELU=0x2du;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_SCALED_TANH=0x2eu;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_SIGMOID=0x2fu;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_SOFTMAX=0x30u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_SOFTPLUS=0x31u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_SOFTSIGN=0x32u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_TANH=0x33u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_THRESHOLDED_RELU=0x34u;
inline constexpr std::uint32_t DML_OPERATOR_CONVOLUTION=0x35u;
inline constexpr std::uint32_t DML_OPERATOR_GEMM=0x36u;
inline constexpr std::uint32_t DML_OPERATOR_REDUCE=0x37u;
inline constexpr std::uint32_t DML_OPERATOR_AVERAGE_POOLING=0x38u;
inline constexpr std::uint32_t DML_OPERATOR_LP_POOLING=0x39u;
inline constexpr std::uint32_t DML_OPERATOR_MAX_POOLING=0x3au;
inline constexpr std::uint32_t DML_OPERATOR_ROI_POOLING=0x3bu;
inline constexpr std::uint32_t DML_OPERATOR_SLICE=0x3cu;
inline constexpr std::uint32_t DML_OPERATOR_CAST=0x3du;
inline constexpr std::uint32_t DML_OPERATOR_SPLIT=0x3eu;
inline constexpr std::uint32_t DML_OPERATOR_JOIN=0x3fu;
inline constexpr std::uint32_t DML_OPERATOR_PADDING=0x40u;
inline constexpr std::uint32_t DML_OPERATOR_VALUE_SCALE_2D=0x41u;
inline constexpr std::uint32_t DML_OPERATOR_UPSAMPLE_2D=0x42u;
inline constexpr std::uint32_t DML_OPERATOR_GATHER=0x43u;
inline constexpr std::uint32_t DML_OPERATOR_SPACE_TO_DEPTH=0x44u;
inline constexpr std::uint32_t DML_OPERATOR_DEPTH_TO_SPACE=0x45u;
inline constexpr std::uint32_t DML_OPERATOR_TILE=0x46u;
inline constexpr std::uint32_t DML_OPERATOR_TOP_K=0x47u;
inline constexpr std::uint32_t DML_OPERATOR_BATCH_NORMALIZATION=0x48u;
inline constexpr std::uint32_t DML_OPERATOR_MEAN_VARIANCE_NORMALIZATION=0x49u;
inline constexpr std::uint32_t DML_OPERATOR_LOCAL_RESPONSE_NORMALIZATION=0x4au;
inline constexpr std::uint32_t DML_OPERATOR_LP_NORMALIZATION=0x4bu;
inline constexpr std::uint32_t DML_OPERATOR_RNN=0x4cu;
inline constexpr std::uint32_t DML_OPERATOR_LSTM=0x4du;
inline constexpr std::uint32_t DML_OPERATOR_GRU=0x4eu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_SIGN=0x4fu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_IS_NAN=0x50u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_ERF=0x51u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_SINH=0x52u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_COSH=0x53u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_TANH=0x54u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_ASINH=0x55u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_ACOSH=0x56u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_ATANH=0x57u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_IF=0x58u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_ADD1=0x59u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_SHRINK=0x5au;
inline constexpr std::uint32_t DML_OPERATOR_MAX_POOLING1=0x5bu;
inline constexpr std::uint32_t DML_OPERATOR_MAX_UNPOOLING=0x5cu;
inline constexpr std::uint32_t DML_OPERATOR_DIAGONAL_MATRIX=0x5du;
inline constexpr std::uint32_t DML_OPERATOR_SCATTER_ELEMENTS=0x5eu;
inline constexpr std::uint32_t DML_OPERATOR_SCATTER=0x5eu;
inline constexpr std::uint32_t DML_OPERATOR_ONE_HOT=0x5fu;
inline constexpr std::uint32_t DML_OPERATOR_RESAMPLE=0x60u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_BIT_SHIFT_LEFT=0x61u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_BIT_SHIFT_RIGHT=0x62u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_ROUND=0x63u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_IS_INFINITY=0x64u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_MODULUS_TRUNCATE=0x65u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_MODULUS_FLOOR=0x66u;
inline constexpr std::uint32_t DML_OPERATOR_FILL_VALUE_CONSTANT=0x67u;
inline constexpr std::uint32_t DML_OPERATOR_FILL_VALUE_SEQUENCE=0x68u;
inline constexpr std::uint32_t DML_OPERATOR_CUMULATIVE_SUMMATION=0x69u;
inline constexpr std::uint32_t DML_OPERATOR_REVERSE_SUBSEQUENCES=0x6au;
inline constexpr std::uint32_t DML_OPERATOR_GATHER_ELEMENTS=0x6bu;
inline constexpr std::uint32_t DML_OPERATOR_GATHER_ND=0x6cu;
inline constexpr std::uint32_t DML_OPERATOR_SCATTER_ND=0x6du;
inline constexpr std::uint32_t DML_OPERATOR_MAX_POOLING2=0x6eu;
inline constexpr std::uint32_t DML_OPERATOR_SLICE1=0x6fu;
inline constexpr std::uint32_t DML_OPERATOR_TOP_K1=0x70u;
inline constexpr std::uint32_t DML_OPERATOR_DEPTH_TO_SPACE1=0x71u;
inline constexpr std::uint32_t DML_OPERATOR_SPACE_TO_DEPTH1=0x72u;
inline constexpr std::uint32_t DML_OPERATOR_MEAN_VARIANCE_NORMALIZATION1=0x73u;
inline constexpr std::uint32_t DML_OPERATOR_RESAMPLE1=0x74u;
inline constexpr std::uint32_t DML_OPERATOR_MATRIX_MULTIPLY_INTEGER=0x75u;
inline constexpr std::uint32_t DML_OPERATOR_QUANTIZED_LINEAR_MATRIX_MULTIPLY=0x76u;
inline constexpr std::uint32_t DML_OPERATOR_CONVOLUTION_INTEGER=0x77u;
inline constexpr std::uint32_t DML_OPERATOR_QUANTIZED_LINEAR_CONVOLUTION=0x78u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_BIT_AND=0x79u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_BIT_OR=0x7au;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_BIT_XOR=0x7bu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_BIT_NOT=0x7cu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_BIT_COUNT=0x7du;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_LOGICAL_GREATER_THAN_OR_EQUAL=0x7eu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_LOGICAL_LESS_THAN_OR_EQUAL=0x7fu;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_CELU=0x80u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_RELU_GRAD=0x81u;
inline constexpr std::uint32_t DML_OPERATOR_AVERAGE_POOLING_GRAD=0x82u;
inline constexpr std::uint32_t DML_OPERATOR_MAX_POOLING_GRAD=0x83u;
inline constexpr std::uint32_t DML_OPERATOR_RANDOM_GENERATOR=0x84u;
inline constexpr std::uint32_t DML_OPERATOR_NONZERO_COORDINATES=0x85u;
inline constexpr std::uint32_t DML_OPERATOR_RESAMPLE_GRAD=0x86u;
inline constexpr std::uint32_t DML_OPERATOR_SLICE_GRAD=0x87u;
inline constexpr std::uint32_t DML_OPERATOR_ADAM_OPTIMIZER=0x88u;
inline constexpr std::uint32_t DML_OPERATOR_ARGMIN=0x89u;
inline constexpr std::uint32_t DML_OPERATOR_ARGMAX=0x8au;
inline constexpr std::uint32_t DML_OPERATOR_ROI_ALIGN=0x8bu;
inline constexpr std::uint32_t DML_OPERATOR_GATHER_ND1=0x8cu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_ATAN_YX=0x8du;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_CLIP_GRAD=0x8eu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_DIFFERENCE_SQUARE=0x8fu;
inline constexpr std::uint32_t DML_OPERATOR_LOCAL_RESPONSE_NORMALIZATION_GRAD=0x90u;
inline constexpr std::uint32_t DML_OPERATOR_CUMULATIVE_PRODUCT=0x91u;
inline constexpr std::uint32_t DML_OPERATOR_BATCH_NORMALIZATION_GRAD=0x92u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_QUANTIZED_LINEAR_ADD=0x93u;
inline constexpr std::uint32_t DML_OPERATOR_DYNAMIC_QUANTIZE_LINEAR=0x94u;
inline constexpr std::uint32_t DML_OPERATOR_ROI_ALIGN1=0x95u;
inline constexpr std::uint32_t DML_OPERATOR_ROI_ALIGN_GRAD=0x96u;
inline constexpr std::uint32_t DML_OPERATOR_BATCH_NORMALIZATION_TRAINING=0x97u;
inline constexpr std::uint32_t DML_OPERATOR_BATCH_NORMALIZATION_TRAINING_GRAD=0x98u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_CLIP1=0x99u;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_CLIP_GRAD1=0x9au;
inline constexpr std::uint32_t DML_OPERATOR_PADDING1=0x9bu;
inline constexpr std::uint32_t DML_OPERATOR_ELEMENT_WISE_NEGATE=0x9cu;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_GELU=0x9du;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_SOFTMAX1=0x9eu;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_LOG_SOFTMAX1=0x9fu;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_HARDMAX1=0xa0u;
inline constexpr std::uint32_t DML_OPERATOR_RESAMPLE2=0xa1u;
inline constexpr std::uint32_t DML_OPERATOR_RESAMPLE_GRAD1=0xa2u;
inline constexpr std::uint32_t DML_OPERATOR_DIAGONAL_MATRIX1=0xa3u;
inline constexpr std::uint32_t DML_OPERATOR_MULTIHEAD_ATTENTION=0xa4u;
inline constexpr std::uint32_t DML_OPERATOR_LP_POOLING1=0xa5u;
inline constexpr std::uint32_t DML_OPERATOR_AVERAGE_POOLING1=0xa6u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_SWISH=0xa7u;
inline constexpr std::uint32_t DML_OPERATOR_ACTIVATION_HARD_SWISH=0xa8u;
inline constexpr std::uint32_t DML_OPERATOR_QUANTIZED_LINEAR_AVERAGE_POOLING=0xa9u;
inline constexpr std::uint32_t DML_OPERATOR_MATRIX_MULTIPLY_INTEGER_TO_FLOAT=0xaau;
inline constexpr std::uint32_t DML_OPERATOR_MEAN_VARIANCE_NORMALIZATION2=0xabu;
inline constexpr std::uint32_t DML_OPERATOR_MULTIHEAD_ATTENTION1=0xacu;
inline constexpr std::uint32_t DML_OPERATOR_QUANTIZE=0xadu;
inline constexpr std::uint32_t DML_OPERATOR_DEQUANTIZE=0xaeu;
inline constexpr std::uint32_t DML_OPERATOR_RESAMPLE3=0xafu;
inline constexpr std::uint32_t DML_OPERATOR_FOLD=0xb0u;
inline constexpr std::uint32_t DML_OPERATOR_UNFOLD=0xb1u;
inline constexpr std::uint32_t DML_REDUCE_FUNCTION_ARGMAX=0x0u;
inline constexpr std::uint32_t DML_REDUCE_FUNCTION_ARGMIN=0x1u;
inline constexpr std::uint32_t DML_REDUCE_FUNCTION_AVERAGE=0x2u;
inline constexpr std::uint32_t DML_REDUCE_FUNCTION_L1=0x3u;
inline constexpr std::uint32_t DML_REDUCE_FUNCTION_L2=0x4u;
inline constexpr std::uint32_t DML_REDUCE_FUNCTION_LOG_SUM=0x5u;
inline constexpr std::uint32_t DML_REDUCE_FUNCTION_LOG_SUM_EXP=0x6u;
inline constexpr std::uint32_t DML_REDUCE_FUNCTION_MAX=0x7u;
inline constexpr std::uint32_t DML_REDUCE_FUNCTION_MIN=0x8u;
inline constexpr std::uint32_t DML_REDUCE_FUNCTION_MULTIPLY=0x9u;
inline constexpr std::uint32_t DML_REDUCE_FUNCTION_SUM=0xau;
inline constexpr std::uint32_t DML_REDUCE_FUNCTION_SUM_SQUARE=0xbu;
inline constexpr std::uint32_t DML_MATRIX_TRANSFORM_NONE=0x0u;
inline constexpr std::uint32_t DML_MATRIX_TRANSFORM_TRANSPOSE=0x1u;
inline constexpr std::uint32_t DML_CONVOLUTION_MODE_CONVOLUTION=0x0u;
inline constexpr std::uint32_t DML_CONVOLUTION_MODE_CROSS_CORRELATION=0x1u;
inline constexpr std::uint32_t DML_CONVOLUTION_DIRECTION_FORWARD=0x0u;
inline constexpr std::uint32_t DML_CONVOLUTION_DIRECTION_BACKWARD=0x1u;
inline constexpr std::uint32_t DML_PADDING_MODE_CONSTANT=0x0u;
inline constexpr std::uint32_t DML_PADDING_MODE_EDGE=0x1u;
inline constexpr std::uint32_t DML_PADDING_MODE_REFLECTION=0x2u;
inline constexpr std::uint32_t DML_PADDING_MODE_SYMMETRIC=0x3u;
inline constexpr std::uint32_t DML_PADDING_MODE_WRAP=0x4u;
inline constexpr std::uint32_t DML_INTERPOLATION_MODE_NEAREST_NEIGHBOR=0x0u;
inline constexpr std::uint32_t DML_INTERPOLATION_MODE_LINEAR=0x1u;
inline constexpr std::uint32_t DML_RECURRENT_NETWORK_DIRECTION_FORWARD=0x0u;
inline constexpr std::uint32_t DML_RECURRENT_NETWORK_DIRECTION_BACKWARD=0x1u;
inline constexpr std::uint32_t DML_RECURRENT_NETWORK_DIRECTION_BIDIRECTIONAL=0x2u;
inline constexpr std::uint32_t DML_ROUNDING_MODE_HALVES_TO_NEAREST_EVEN=0x0u;
inline constexpr std::uint32_t DML_ROUNDING_MODE_TOWARD_ZERO=0x1u;
inline constexpr std::uint32_t DML_ROUNDING_MODE_TOWARD_INFINITY=0x2u;
inline constexpr std::uint32_t DML_IS_INFINITY_MODE_EITHER=0x0u;
inline constexpr std::uint32_t DML_IS_INFINITY_MODE_POSITIVE=0x1u;
inline constexpr std::uint32_t DML_IS_INFINITY_MODE_NEGATIVE=0x2u;
inline constexpr std::uint32_t DML_AXIS_DIRECTION_INCREASING=0x0u;
inline constexpr std::uint32_t DML_AXIS_DIRECTION_DECREASING=0x1u;
inline constexpr std::uint32_t DML_DEPTH_SPACE_ORDER_DEPTH_COLUMN_ROW=0x0u;
inline constexpr std::uint32_t DML_DEPTH_SPACE_ORDER_COLUMN_ROW_DEPTH=0x1u;
inline constexpr std::uint32_t DML_RANDOM_GENERATOR_TYPE_PHILOX_4X32_10=0x0u;
inline constexpr std::uint32_t DML_MULTIHEAD_ATTENTION_MASK_TYPE_NONE=0x0u;
inline constexpr std::uint32_t DML_MULTIHEAD_ATTENTION_MASK_TYPE_KEY_SEQUENCE_LENGTH=0x1u;
inline constexpr std::uint32_t DML_MULTIHEAD_ATTENTION_MASK_TYPE_KEY_SEQUENCE_END_START=0x2u;
inline constexpr std::uint32_t DML_MULTIHEAD_ATTENTION_MASK_TYPE_KEY_QUERY_SEQUENCE_LENGTH_START_END=0x3u;
inline constexpr std::uint32_t DML_MULTIHEAD_ATTENTION_MASK_TYPE_BOOLEAN=0x4u;
inline constexpr std::uint32_t DML_QUANTIZATION_TYPE_NONE=0x0u;
inline constexpr std::uint32_t DML_QUANTIZATION_TYPE_SCALE=0x1u;
inline constexpr std::uint32_t DML_QUANTIZATION_TYPE_SCALE_ZERO_POINT=0x2u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_1_0=0x1000u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_2_0=0x2000u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_2_1=0x2100u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_3_0=0x3000u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_3_1=0x3100u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_4_0=0x4000u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_4_1=0x4100u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_5_0=0x5000u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_5_1=0x5100u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_5_2=0x5200u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_6_0=0x6000u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_6_1=0x6100u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_6_2=0x6200u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_6_3=0x6300u;
inline constexpr std::uint32_t DML_FEATURE_LEVEL_6_4=0x6400u;
inline constexpr std::uint32_t DML_FEATURE_TENSOR_DATA_TYPE_SUPPORT=0x0u;
inline constexpr std::uint32_t DML_FEATURE_FEATURE_LEVELS=0x1u;
inline constexpr std::uint32_t DML_EXECUTION_FLAG_NONE=0x0u;
inline constexpr std::uint32_t DML_EXECUTION_FLAG_ALLOW_HALF_PRECISION_COMPUTATION=0x1u;
inline constexpr std::uint32_t DML_EXECUTION_FLAG_DISABLE_META_COMMANDS=0x2u;
inline constexpr std::uint32_t DML_EXECUTION_FLAG_DESCRIPTORS_VOLATILE=0x4u;
inline constexpr std::uint32_t DML_CREATE_DEVICE_FLAG_NONE=0x0u;
inline constexpr std::uint32_t DML_CREATE_DEVICE_FLAG_DEBUG=0x1u;
inline constexpr std::uint32_t DML_BINDING_TYPE_NONE=0x0u;
inline constexpr std::uint32_t DML_BINDING_TYPE_BUFFER=0x1u;
inline constexpr std::uint32_t DML_BINDING_TYPE_BUFFER_ARRAY=0x2u;
inline constexpr std::uint32_t DML_GRAPH_EDGE_TYPE_INVALID=0x0u;
inline constexpr std::uint32_t DML_GRAPH_EDGE_TYPE_INPUT=0x1u;
inline constexpr std::uint32_t DML_GRAPH_EDGE_TYPE_OUTPUT=0x2u;
inline constexpr std::uint32_t DML_GRAPH_EDGE_TYPE_INTERMEDIATE=0x3u;
inline constexpr std::uint32_t DML_GRAPH_NODE_TYPE_INVALID=0x0u;
inline constexpr std::uint32_t DML_GRAPH_NODE_TYPE_OPERATOR=0x1u;
inline constexpr std::uint32_t DML_GRAPH_NODE_TYPE_CONSTANT=0x2u;
inline constexpr OperatorField DML_ELEMENT_WISE_IDENTITY_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_ABS_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_ACOS_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_ADD_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_ASIN_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_ATAN_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_CEIL_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_CLIP_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true},{"Min",2,7,24,-1,false},{"Max",2,7,28,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_CLIP1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true},{"MinMaxDataType",2,4,24,-1,false},{"Min",2,13,32,-1,false},{"Max",2,13,40,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_CLIP_GRAD_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"InputGradientTensor",0,0,8,-1,false},{"OutputGradientTensor",1,0,16,-1,false},{"Min",2,7,24,-1,false},{"Max",2,7,28,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_CLIP_GRAD1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"InputGradientTensor",0,0,8,-1,false},{"OutputGradientTensor",1,0,16,-1,false},{"MinMaxDataType",2,4,24,-1,false},{"Min",2,13,32,-1,false},{"Max",2,13,40,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_COS_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_DIVIDE_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_EXP_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_FLOOR_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_LOG_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_LOGICAL_AND_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_LOGICAL_EQUALS_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_LOGICAL_GREATER_THAN_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_LOGICAL_LESS_THAN_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_LOGICAL_GREATER_THAN_OR_EQUAL_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_LOGICAL_LESS_THAN_OR_EQUAL_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_LOGICAL_NOT_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_LOGICAL_OR_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_LOGICAL_XOR_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_MAX_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_MEAN_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_MIN_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_MULTIPLY_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_POW_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"ExponentTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false},{"ScaleBias",2,11,24,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_CONSTANT_POW_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true},{"Exponent",2,7,24,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_RECIP_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_SIN_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_SQRT_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_DIFFERENCE_SQUARE_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_ATAN_YX_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_SUBTRACT_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_TAN_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_THRESHOLD_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true},{"Min",2,7,24,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_QUANTIZE_LINEAR_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"ScaleTensor",0,0,8,-1,false},{"ZeroPointTensor",0,0,16,-1,true},{"OutputTensor",1,0,24,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_DEQUANTIZE_LINEAR_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"ScaleTensor",0,0,8,-1,false},{"ZeroPointTensor",0,0,16,-1,true},{"OutputTensor",1,0,24,-1,false}};
inline constexpr OperatorField DML_CONVOLUTION_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"FilterTensor",0,0,8,-1,false},{"BiasTensor",0,0,16,-1,true},{"OutputTensor",1,0,24,-1,false},{"Mode",2,4,32,-1,false},{"Direction",2,4,36,-1,false},{"DimensionCount",2,4,40,-1,false},{"Strides",2,8,48,6,false},{"Dilations",2,8,56,6,false},{"StartPadding",2,8,64,6,false},{"EndPadding",2,8,72,6,false},{"OutputPadding",2,8,80,6,false},{"GroupCount",2,4,88,-1,false},{"FusedActivation",2,2,96,-1,true}};
inline constexpr OperatorField DML_GEMM_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"CTensor",0,0,16,-1,true},{"OutputTensor",1,0,24,-1,false},{"TransA",2,4,32,-1,false},{"TransB",2,4,36,-1,false},{"Alpha",2,7,40,-1,false},{"Beta",2,7,44,-1,false},{"FusedActivation",2,2,48,-1,true}};
inline constexpr OperatorField DML_REDUCE_OPERATOR_fields[]{{"Function",2,4,0,-1,false},{"InputTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false},{"AxisCount",2,4,24,-1,false},{"Axes",2,8,32,3,false}};
inline constexpr OperatorField DML_ARGMIN_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"AxisCount",2,4,16,-1,false},{"Axes",2,8,24,2,false},{"AxisDirection",2,4,32,-1,false}};
inline constexpr OperatorField DML_ARGMAX_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"AxisCount",2,4,16,-1,false},{"Axes",2,8,24,2,false},{"AxisDirection",2,4,32,-1,false}};
inline constexpr OperatorField DML_AVERAGE_POOLING_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"DimensionCount",2,4,16,-1,false},{"Strides",2,8,24,2,false},{"WindowSize",2,8,32,2,false},{"StartPadding",2,8,40,2,false},{"EndPadding",2,8,48,2,false},{"IncludePadding",2,4,56,-1,false}};
inline constexpr OperatorField DML_AVERAGE_POOLING1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"DimensionCount",2,4,16,-1,false},{"Strides",2,8,24,2,false},{"WindowSize",2,8,32,2,false},{"StartPadding",2,8,40,2,false},{"EndPadding",2,8,48,2,false},{"Dilations",2,8,56,2,false},{"IncludePadding",2,4,64,-1,false}};
inline constexpr OperatorField DML_LP_POOLING_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"DimensionCount",2,4,16,-1,false},{"Strides",2,8,24,2,false},{"WindowSize",2,8,32,2,false},{"StartPadding",2,8,40,2,false},{"EndPadding",2,8,48,2,false},{"P",2,4,56,-1,false}};
inline constexpr OperatorField DML_LP_POOLING1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"DimensionCount",2,4,16,-1,false},{"Strides",2,8,24,2,false},{"WindowSize",2,8,32,2,false},{"StartPadding",2,8,40,2,false},{"EndPadding",2,8,48,2,false},{"Dilations",2,8,56,2,false},{"P",2,4,64,-1,false}};
inline constexpr OperatorField DML_MAX_POOLING_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"DimensionCount",2,4,16,-1,false},{"Strides",2,8,24,2,false},{"WindowSize",2,8,32,2,false},{"StartPadding",2,8,40,2,false},{"EndPadding",2,8,48,2,false}};
inline constexpr OperatorField DML_MAX_POOLING1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"OutputIndicesTensor",1,0,16,-1,true},{"DimensionCount",2,4,24,-1,false},{"Strides",2,8,32,3,false},{"WindowSize",2,8,40,3,false},{"StartPadding",2,8,48,3,false},{"EndPadding",2,8,56,3,false}};
inline constexpr OperatorField DML_ROI_POOLING_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"ROITensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false},{"SpatialScale",2,7,24,-1,false},{"PooledSize",2,12,28,-1,false}};
inline constexpr OperatorField DML_SLICE_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"DimensionCount",2,4,16,-1,false},{"Offsets",2,8,24,2,false},{"Sizes",2,8,32,2,false},{"Strides",2,8,40,2,false}};
inline constexpr OperatorField DML_CAST_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_SPLIT_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputCount",2,4,8,-1,false},{"OutputTensors",1,1,16,1,false},{"Axis",2,4,24,-1,false}};
inline constexpr OperatorField DML_JOIN_OPERATOR_fields[]{{"InputCount",2,4,0,-1,false},{"InputTensors",0,1,8,0,false},{"OutputTensor",1,0,16,-1,false},{"Axis",2,4,24,-1,false}};
inline constexpr OperatorField DML_PADDING_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"PaddingMode",2,4,16,-1,false},{"PaddingValue",2,7,20,-1,false},{"DimensionCount",2,4,24,-1,false},{"StartPadding",2,8,32,4,false},{"EndPadding",2,8,40,4,false}};
inline constexpr OperatorField DML_PADDING1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"PaddingMode",2,4,16,-1,false},{"PaddingValueDataType",2,4,20,-1,false},{"PaddingValue",2,13,24,-1,false},{"DimensionCount",2,4,32,-1,false},{"StartPadding",2,8,40,5,false},{"EndPadding",2,8,48,5,false}};
inline constexpr OperatorField DML_VALUE_SCALE_2D_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Scale",2,7,16,-1,false},{"ChannelCount",2,4,20,-1,false},{"Bias",2,10,24,3,false}};
inline constexpr OperatorField DML_UPSAMPLE_2D_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleSize",2,12,16,-1,false},{"InterpolationMode",2,4,24,-1,false}};
inline constexpr OperatorField DML_GATHER_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"IndicesTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false},{"Axis",2,4,24,-1,false},{"IndexDimensions",2,4,28,-1,false}};
inline constexpr OperatorField DML_SPACE_TO_DEPTH_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"BlockSize",2,4,16,-1,false}};
inline constexpr OperatorField DML_DEPTH_TO_SPACE_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"BlockSize",2,4,16,-1,false}};
inline constexpr OperatorField DML_TILE_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"RepeatsCount",2,4,16,-1,false},{"Repeats",2,8,24,2,false}};
inline constexpr OperatorField DML_TOP_K_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputValueTensor",1,0,8,-1,false},{"OutputIndexTensor",1,0,16,-1,false},{"Axis",2,4,24,-1,false},{"K",2,4,28,-1,false}};
inline constexpr OperatorField DML_BATCH_NORMALIZATION_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"MeanTensor",0,0,8,-1,false},{"VarianceTensor",0,0,16,-1,false},{"ScaleTensor",0,0,24,-1,false},{"BiasTensor",0,0,32,-1,false},{"OutputTensor",1,0,40,-1,false},{"Spatial",2,4,48,-1,false},{"Epsilon",2,7,52,-1,false},{"FusedActivation",2,2,56,-1,true}};
inline constexpr OperatorField DML_BATCH_NORMALIZATION_GRAD_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"InputGradientTensor",0,0,8,-1,false},{"MeanTensor",0,0,16,-1,false},{"VarianceTensor",0,0,24,-1,false},{"ScaleTensor",0,0,32,-1,false},{"OutputGradientTensor",1,0,40,-1,false},{"OutputScaleGradientTensor",1,0,48,-1,false},{"OutputBiasGradientTensor",1,0,56,-1,false},{"Epsilon",2,7,64,-1,true}};
inline constexpr OperatorField DML_BATCH_NORMALIZATION_TRAINING_GRAD_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"InputGradientTensor",0,0,8,-1,false},{"MeanTensor",0,0,16,-1,false},{"VarianceTensor",0,0,24,-1,false},{"ScaleTensor",0,0,32,-1,false},{"OutputGradientTensor",1,0,40,-1,false},{"OutputScaleGradientTensor",1,0,48,-1,false},{"OutputBiasGradientTensor",1,0,56,-1,false},{"Epsilon",2,7,64,-1,false}};
inline constexpr OperatorField DML_MEAN_VARIANCE_NORMALIZATION_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"ScaleTensor",0,0,8,-1,true},{"BiasTensor",0,0,16,-1,true},{"OutputTensor",1,0,24,-1,false},{"CrossChannel",2,4,32,-1,false},{"NormalizeVariance",2,4,36,-1,false},{"Epsilon",2,7,40,-1,false},{"FusedActivation",2,2,48,-1,true}};
inline constexpr OperatorField DML_LOCAL_RESPONSE_NORMALIZATION_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"CrossChannel",2,4,16,-1,false},{"LocalSize",2,4,20,-1,false},{"Alpha",2,7,24,-1,false},{"Beta",2,7,28,-1,false},{"Bias",2,7,32,-1,false}};
inline constexpr OperatorField DML_LOCAL_RESPONSE_NORMALIZATION_GRAD_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"InputGradientTensor",0,0,8,-1,false},{"OutputGradientTensor",1,0,16,-1,false},{"CrossChannel",2,14,24,-1,false},{"LocalSize",2,4,28,-1,false},{"Alpha",2,7,32,-1,false},{"Beta",2,7,36,-1,false},{"Bias",2,7,40,-1,false}};
inline constexpr OperatorField DML_LP_NORMALIZATION_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Axis",2,4,16,-1,false},{"Epsilon",2,7,20,-1,false},{"P",2,4,24,-1,false}};
inline constexpr OperatorField DML_RNN_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"WeightTensor",0,0,8,-1,false},{"RecurrenceTensor",0,0,16,-1,false},{"BiasTensor",0,0,24,-1,true},{"HiddenInitTensor",0,0,32,-1,true},{"SequenceLengthsTensor",0,0,40,-1,true},{"OutputSequenceTensor",1,0,48,-1,true},{"OutputSingleTensor",1,0,56,-1,true},{"ActivationDescCount",2,4,64,-1,false},{"ActivationDescs",2,3,72,8,false},{"Direction",2,4,80,-1,false}};
inline constexpr OperatorField DML_LSTM_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"WeightTensor",0,0,8,-1,false},{"RecurrenceTensor",0,0,16,-1,false},{"BiasTensor",0,0,24,-1,true},{"HiddenInitTensor",0,0,32,-1,true},{"CellMemInitTensor",0,0,40,-1,true},{"SequenceLengthsTensor",0,0,48,-1,true},{"PeepholeTensor",0,0,56,-1,true},{"OutputSequenceTensor",1,0,64,-1,true},{"OutputSingleTensor",1,0,72,-1,true},{"OutputCellSingleTensor",1,0,80,-1,true},{"ActivationDescCount",2,4,88,-1,false},{"ActivationDescs",2,3,96,11,false},{"Direction",2,4,104,-1,false},{"ClipThreshold",2,7,108,-1,false},{"UseClipThreshold",2,4,112,-1,false},{"CoupleInputForget",2,4,116,-1,false}};
inline constexpr OperatorField DML_GRU_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"WeightTensor",0,0,8,-1,false},{"RecurrenceTensor",0,0,16,-1,false},{"BiasTensor",0,0,24,-1,true},{"HiddenInitTensor",0,0,32,-1,true},{"SequenceLengthsTensor",0,0,40,-1,true},{"OutputSequenceTensor",1,0,48,-1,true},{"OutputSingleTensor",1,0,56,-1,true},{"ActivationDescCount",2,4,64,-1,false},{"ActivationDescs",2,3,72,8,false},{"Direction",2,4,80,-1,false},{"LinearBeforeReset",2,4,84,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_SIGN_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_IS_NAN_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_NEGATE_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_ERF_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_SINH_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_COSH_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_TANH_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_ASINH_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_ACOSH_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_ATANH_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"ScaleBias",2,11,16,-1,true}};
inline constexpr OperatorField DML_ELEMENT_WISE_IF_OPERATOR_fields[]{{"ConditionTensor",0,0,0,-1,false},{"ATensor",0,0,8,-1,false},{"BTensor",0,0,16,-1,false},{"OutputTensor",1,0,24,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_ADD1_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false},{"FusedActivation",2,2,24,-1,true}};
inline constexpr OperatorField DML_MAX_UNPOOLING_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"IndicesTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_DIAGONAL_MATRIX_OPERATOR_fields[]{{"OutputTensor",1,0,0,-1,false},{"Offset",2,6,8,-1,false},{"Value",2,7,12,-1,false}};
inline constexpr OperatorField DML_SCATTER_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"IndicesTensor",0,0,8,-1,false},{"UpdatesTensor",0,0,16,-1,false},{"OutputTensor",1,0,24,-1,false},{"Axis",2,4,32,-1,false}};
inline constexpr OperatorField DML_ONE_HOT_OPERATOR_fields[]{{"IndicesTensor",0,0,0,-1,false},{"ValuesTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false},{"Axis",2,4,24,-1,false}};
inline constexpr OperatorField DML_RESAMPLE_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"InterpolationMode",2,4,16,-1,false},{"ScaleCount",2,4,20,-1,false},{"Scales",2,10,24,3,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_BIT_SHIFT_LEFT_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_BIT_SHIFT_RIGHT_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_ROUND_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"RoundingMode",2,4,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_IS_INFINITY_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"InfinityMode",2,4,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_MODULUS_TRUNCATE_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_MODULUS_FLOOR_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_FILL_VALUE_CONSTANT_OPERATOR_fields[]{{"OutputTensor",1,0,0,-1,false},{"ValueDataType",2,4,8,-1,false},{"Value",2,13,16,-1,false}};
inline constexpr OperatorField DML_FILL_VALUE_SEQUENCE_OPERATOR_fields[]{{"OutputTensor",1,0,0,-1,false},{"ValueDataType",2,4,8,-1,false},{"ValueStart",2,13,16,-1,false},{"ValueDelta",2,13,24,-1,false}};
inline constexpr OperatorField DML_CUMULATIVE_SUMMATION_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Axis",2,4,16,-1,false},{"AxisDirection",2,4,20,-1,false},{"HasExclusiveSum",2,4,24,-1,false}};
inline constexpr OperatorField DML_CUMULATIVE_PRODUCT_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Axis",2,4,16,-1,false},{"AxisDirection",2,4,20,-1,false},{"HasExclusiveProduct",2,4,24,-1,false}};
inline constexpr OperatorField DML_REVERSE_SUBSEQUENCES_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"SequenceLengthsTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false},{"Axis",2,4,24,-1,false}};
inline constexpr OperatorField DML_GATHER_ELEMENTS_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"IndicesTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false},{"Axis",2,4,24,-1,false}};
inline constexpr OperatorField DML_GATHER_ND_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"IndicesTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false},{"InputDimensionCount",2,4,24,-1,false},{"IndicesDimensionCount",2,4,28,-1,false}};
inline constexpr OperatorField DML_SCATTER_ND_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"IndicesTensor",0,0,8,-1,false},{"UpdatesTensor",0,0,16,-1,false},{"OutputTensor",1,0,24,-1,false},{"InputDimensionCount",2,4,32,-1,false},{"IndicesDimensionCount",2,4,36,-1,false}};
inline constexpr OperatorField DML_MAX_POOLING2_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"OutputIndicesTensor",1,0,16,-1,true},{"DimensionCount",2,4,24,-1,false},{"Strides",2,8,32,3,false},{"WindowSize",2,8,40,3,false},{"StartPadding",2,8,48,3,false},{"EndPadding",2,8,56,3,false},{"Dilations",2,8,64,3,false}};
inline constexpr OperatorField DML_SLICE1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"DimensionCount",2,4,16,-1,false},{"InputWindowOffsets",2,8,24,2,false},{"InputWindowSizes",2,8,32,2,false},{"InputWindowStrides",2,9,40,2,false}};
inline constexpr OperatorField DML_TOP_K1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputValueTensor",1,0,8,-1,false},{"OutputIndexTensor",1,0,16,-1,false},{"Axis",2,4,24,-1,false},{"K",2,4,28,-1,false},{"AxisDirection",2,4,32,-1,false}};
inline constexpr OperatorField DML_DEPTH_TO_SPACE1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"BlockSize",2,4,16,-1,false},{"Order",2,4,20,-1,false}};
inline constexpr OperatorField DML_SPACE_TO_DEPTH1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"BlockSize",2,4,16,-1,false},{"Order",2,4,20,-1,false}};
inline constexpr OperatorField DML_MEAN_VARIANCE_NORMALIZATION1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"ScaleTensor",0,0,8,-1,true},{"BiasTensor",0,0,16,-1,true},{"OutputTensor",1,0,24,-1,false},{"AxisCount",2,4,32,-1,false},{"Axes",2,8,40,4,false},{"NormalizeVariance",2,4,48,-1,false},{"Epsilon",2,7,52,-1,false},{"FusedActivation",2,2,56,-1,true}};
inline constexpr OperatorField DML_RESAMPLE1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"InterpolationMode",2,4,16,-1,false},{"DimensionCount",2,4,20,-1,false},{"Scales",2,10,24,3,false},{"InputPixelOffsets",2,10,32,3,false},{"OutputPixelOffsets",2,10,40,3,false}};
inline constexpr OperatorField DML_MATRIX_MULTIPLY_INTEGER_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"AZeroPointTensor",0,0,8,-1,true},{"BTensor",0,0,16,-1,false},{"BZeroPointTensor",0,0,24,-1,true},{"OutputTensor",1,0,32,-1,false}};
inline constexpr OperatorField DML_QUANTIZED_LINEAR_MATRIX_MULTIPLY_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"AScaleTensor",0,0,8,-1,false},{"AZeroPointTensor",0,0,16,-1,true},{"BTensor",0,0,24,-1,false},{"BScaleTensor",0,0,32,-1,false},{"BZeroPointTensor",0,0,40,-1,true},{"OutputScaleTensor",0,0,48,-1,false},{"OutputZeroPointTensor",0,0,56,-1,true},{"OutputTensor",1,0,64,-1,false}};
inline constexpr OperatorField DML_CONVOLUTION_INTEGER_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"InputZeroPointTensor",0,0,8,-1,true},{"FilterTensor",0,0,16,-1,false},{"FilterZeroPointTensor",0,0,24,-1,true},{"OutputTensor",1,0,32,-1,false},{"DimensionCount",2,4,40,-1,false},{"Strides",2,8,48,5,false},{"Dilations",2,8,56,5,false},{"StartPadding",2,8,64,5,false},{"EndPadding",2,8,72,5,false},{"GroupCount",2,4,80,-1,false}};
inline constexpr OperatorField DML_QUANTIZED_LINEAR_CONVOLUTION_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"InputScaleTensor",0,0,8,-1,false},{"InputZeroPointTensor",0,0,16,-1,true},{"FilterTensor",0,0,24,-1,false},{"FilterScaleTensor",0,0,32,-1,false},{"FilterZeroPointTensor",0,0,40,-1,true},{"BiasTensor",0,0,48,-1,true},{"OutputScaleTensor",0,0,56,-1,false},{"OutputZeroPointTensor",0,0,64,-1,true},{"OutputTensor",1,0,72,-1,false},{"DimensionCount",2,4,80,-1,false},{"Strides",2,8,88,10,false},{"Dilations",2,8,96,10,false},{"StartPadding",2,8,104,10,false},{"EndPadding",2,8,112,10,false},{"GroupCount",2,4,120,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_BIT_AND_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_BIT_OR_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_BIT_XOR_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"BTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_BIT_NOT_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_BIT_COUNT_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_RELU_GRAD_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"InputGradientTensor",0,0,8,-1,false},{"OutputGradientTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_AVERAGE_POOLING_GRAD_OPERATOR_fields[]{{"InputGradientTensor",0,0,0,-1,false},{"OutputGradientTensor",1,0,8,-1,false},{"DimensionCount",2,4,16,-1,false},{"Strides",2,8,24,2,false},{"WindowSize",2,8,32,2,false},{"StartPadding",2,8,40,2,false},{"EndPadding",2,8,48,2,false},{"IncludePadding",2,4,56,-1,false}};
inline constexpr OperatorField DML_MAX_POOLING_GRAD_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"InputGradientTensor",0,0,8,-1,false},{"OutputGradientTensor",1,0,16,-1,false},{"DimensionCount",2,4,24,-1,false},{"Strides",2,8,32,3,false},{"WindowSize",2,8,40,3,false},{"StartPadding",2,8,48,3,false},{"EndPadding",2,8,56,3,false},{"Dilations",2,8,64,3,false}};
inline constexpr OperatorField DML_RANDOM_GENERATOR_OPERATOR_fields[]{{"InputStateTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"OutputStateTensor",1,0,16,-1,true},{"Type",2,4,24,-1,false}};
inline constexpr OperatorField DML_NONZERO_COORDINATES_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputCountTensor",1,0,8,-1,false},{"OutputCoordinatesTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_RESAMPLE_GRAD_OPERATOR_fields[]{{"InputGradientTensor",0,0,0,-1,false},{"OutputGradientTensor",1,0,8,-1,false},{"InterpolationMode",2,4,16,-1,false},{"DimensionCount",2,4,20,-1,false},{"Scales",2,10,24,3,false},{"InputPixelOffsets",2,10,32,3,false},{"OutputPixelOffsets",2,10,40,3,false}};
inline constexpr OperatorField DML_SLICE_GRAD_OPERATOR_fields[]{{"InputGradientTensor",0,0,0,-1,false},{"OutputGradientTensor",1,0,8,-1,false},{"DimensionCount",2,4,16,-1,false},{"InputWindowOffsets",2,8,24,2,false},{"InputWindowSizes",2,8,32,2,false},{"InputWindowStrides",2,9,40,2,false}};
inline constexpr OperatorField DML_ADAM_OPTIMIZER_OPERATOR_fields[]{{"InputParametersTensor",0,0,0,-1,false},{"InputFirstMomentTensor",0,0,8,-1,false},{"InputSecondMomentTensor",0,0,16,-1,false},{"GradientTensor",0,0,24,-1,false},{"TrainingStepTensor",0,0,32,-1,false},{"OutputParametersTensor",1,0,40,-1,false},{"OutputFirstMomentTensor",1,0,48,-1,false},{"OutputSecondMomentTensor",1,0,56,-1,false},{"LearningRate",2,7,64,-1,false},{"Beta1",2,7,68,-1,false},{"Beta2",2,7,72,-1,false},{"Epsilon",2,7,76,-1,false}};
inline constexpr OperatorField DML_ROI_ALIGN_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"ROITensor",0,0,8,-1,false},{"BatchIndicesTensor",0,0,16,-1,false},{"OutputTensor",1,0,24,-1,false},{"ReductionFunction",2,4,32,-1,false},{"InterpolationMode",2,4,36,-1,false},{"SpatialScaleX",2,7,40,-1,false},{"SpatialScaleY",2,7,44,-1,false},{"OutOfBoundsInputValue",2,7,48,-1,false},{"MinimumSamplesPerOutput",2,4,52,-1,false},{"MaximumSamplesPerOutput",2,4,56,-1,false}};
inline constexpr OperatorField DML_ROI_ALIGN1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"ROITensor",0,0,8,-1,false},{"BatchIndicesTensor",0,0,16,-1,false},{"OutputTensor",1,0,24,-1,false},{"ReductionFunction",2,4,32,-1,false},{"InterpolationMode",2,4,36,-1,false},{"SpatialScaleX",2,7,40,-1,false},{"SpatialScaleY",2,7,44,-1,false},{"InputPixelOffset",2,7,48,-1,false},{"OutputPixelOffset",2,7,52,-1,false},{"OutOfBoundsInputValue",2,7,56,-1,false},{"MinimumSamplesPerOutput",2,4,60,-1,false},{"MaximumSamplesPerOutput",2,4,64,-1,false},{"AlignRegionsToCorners",2,4,68,-1,false}};
inline constexpr OperatorField DML_GATHER_ND1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"IndicesTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false},{"InputDimensionCount",2,4,24,-1,false},{"IndicesDimensionCount",2,4,28,-1,false},{"BatchDimensionCount",2,4,32,-1,false}};
inline constexpr OperatorField DML_DYNAMIC_QUANTIZE_LINEAR_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"OutputScaleTensor",1,0,16,-1,false},{"OutputZeroPointTensor",1,0,24,-1,false}};
inline constexpr OperatorField DML_ELEMENT_WISE_QUANTIZED_LINEAR_ADD_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"AScaleTensor",0,0,8,-1,false},{"AZeroPointTensor",0,0,16,-1,true},{"BTensor",0,0,24,-1,false},{"BScaleTensor",0,0,32,-1,false},{"BZeroPointTensor",0,0,40,-1,true},{"OutputScaleTensor",0,0,48,-1,false},{"OutputZeroPointTensor",0,0,56,-1,true},{"OutputTensor",1,0,64,-1,false}};
inline constexpr OperatorField DML_ROI_ALIGN_GRAD_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,true},{"InputGradientTensor",0,0,8,-1,false},{"ROITensor",0,0,16,-1,false},{"BatchIndicesTensor",0,0,24,-1,false},{"OutputGradientTensor",1,0,32,-1,true},{"OutputROIGradientTensor",1,0,40,-1,true},{"ReductionFunction",2,4,48,-1,false},{"InterpolationMode",2,4,52,-1,false},{"SpatialScaleX",2,7,56,-1,false},{"SpatialScaleY",2,7,60,-1,false},{"InputPixelOffset",2,7,64,-1,false},{"OutputPixelOffset",2,7,68,-1,false},{"MinimumSamplesPerOutput",2,4,72,-1,false},{"MaximumSamplesPerOutput",2,4,76,-1,false},{"AlignRegionsToCorners",2,4,80,-1,false}};
inline constexpr OperatorField DML_BATCH_NORMALIZATION_TRAINING_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"ScaleTensor",0,0,8,-1,false},{"BiasTensor",0,0,16,-1,false},{"FusedAddTensor",0,0,24,-1,true},{"OutputTensor",1,0,32,-1,false},{"OutputMeanTensor",1,0,40,-1,false},{"OutputVarianceTensor",1,0,48,-1,false},{"Epsilon",2,7,56,-1,false},{"FusedActivation",2,2,64,-1,true}};
inline constexpr OperatorField DML_RESAMPLE2_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"InterpolationMode",2,4,16,-1,false},{"RoundingDirection",2,4,20,-1,false},{"DimensionCount",2,4,24,-1,false},{"Scales",2,10,32,4,false},{"InputPixelOffsets",2,10,40,4,false},{"OutputPixelOffsets",2,10,48,4,false}};
inline constexpr OperatorField DML_RESAMPLE_GRAD1_OPERATOR_fields[]{{"InputGradientTensor",0,0,0,-1,false},{"OutputGradientTensor",1,0,8,-1,false},{"InterpolationMode",2,4,16,-1,false},{"RoundingDirection",2,4,20,-1,false},{"DimensionCount",2,4,24,-1,false},{"Scales",2,10,32,4,false},{"InputPixelOffsets",2,10,40,4,false},{"OutputPixelOffsets",2,10,48,4,false}};
inline constexpr OperatorField DML_DIAGONAL_MATRIX1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,true},{"OutputTensor",1,0,8,-1,false},{"ValueDataType",2,4,16,-1,false},{"Value",2,13,24,-1,false},{"DiagonalFillBegin",2,6,32,-1,false},{"DiagonalFillEnd",2,6,36,-1,false}};
inline constexpr OperatorField DML_MULTIHEAD_ATTENTION_OPERATOR_fields[]{{"QueryTensor",0,0,0,-1,true},{"KeyTensor",0,0,8,-1,true},{"ValueTensor",0,0,16,-1,true},{"StackedQueryKeyTensor",0,0,24,-1,true},{"StackedKeyValueTensor",0,0,32,-1,true},{"StackedQueryKeyValueTensor",0,0,40,-1,true},{"BiasTensor",0,0,48,-1,true},{"MaskTensor",0,0,56,-1,true},{"RelativePositionBiasTensor",0,0,64,-1,true},{"PastKeyTensor",0,0,72,-1,true},{"PastValueTensor",0,0,80,-1,true},{"OutputTensor",1,0,88,-1,false},{"OutputPresentKeyTensor",1,0,96,-1,true},{"OutputPresentValueTensor",1,0,104,-1,true},{"Scale",2,7,112,-1,false},{"MaskFilterValue",2,7,116,-1,false},{"HeadCount",2,4,120,-1,false},{"MaskType",2,4,124,-1,false}};
inline constexpr OperatorField DML_QUANTIZED_LINEAR_AVERAGE_POOLING_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"InputScaleTensor",0,0,8,-1,false},{"InputZeroPointTensor",0,0,16,-1,true},{"OutputScaleTensor",0,0,24,-1,false},{"OutputZeroPointTensor",0,0,32,-1,true},{"OutputTensor",1,0,40,-1,false},{"DimensionCount",2,4,48,-1,false},{"Strides",2,8,56,6,false},{"WindowSize",2,8,64,6,false},{"StartPadding",2,8,72,6,false},{"EndPadding",2,8,80,6,false},{"Dilations",2,8,88,6,false},{"IncludePadding",2,4,96,-1,false}};
inline constexpr OperatorField DML_MATRIX_MULTIPLY_INTEGER_TO_FLOAT_OPERATOR_fields[]{{"ATensor",0,0,0,-1,false},{"AScaleTensor",0,0,8,-1,false},{"AZeroPointTensor",0,0,16,-1,true},{"BTensor",0,0,24,-1,false},{"BScaleTensor",0,0,32,-1,false},{"BZeroPointTensor",0,0,40,-1,true},{"BiasTensor",0,0,48,-1,true},{"OutputTensor",1,0,56,-1,false}};
inline constexpr OperatorField DML_MEAN_VARIANCE_NORMALIZATION2_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"ScaleTensor",0,0,8,-1,true},{"BiasTensor",0,0,16,-1,true},{"OutputTensor",1,0,24,-1,false},{"AxisCount",2,4,32,-1,false},{"Axes",2,8,40,4,false},{"UseMean",2,4,48,-1,false},{"UseVariance",2,4,52,-1,false},{"Epsilon",2,7,56,-1,false},{"FusedActivation",2,2,64,-1,true}};
inline constexpr OperatorField DML_MULTIHEAD_ATTENTION1_OPERATOR_fields[]{{"QueryTensor",0,0,0,-1,true},{"KeyTensor",0,0,8,-1,true},{"ValueTensor",0,0,16,-1,true},{"StackedQueryKeyTensor",0,0,24,-1,true},{"StackedKeyValueTensor",0,0,32,-1,true},{"StackedQueryKeyValueTensor",0,0,40,-1,true},{"BiasTensor",0,0,48,-1,true},{"MaskTensor",0,0,56,-1,true},{"RelativePositionBiasTensor",0,0,64,-1,true},{"PastKeyTensor",0,0,72,-1,true},{"PastValueTensor",0,0,80,-1,true},{"PastSequenceLengthsTensor",0,0,88,-1,true},{"OutputTensor",1,0,96,-1,false},{"OutputPresentKeyTensor",1,0,104,-1,true},{"OutputPresentValueTensor",1,0,112,-1,true},{"Scale",2,7,120,-1,false},{"MaskFilterValue",2,7,124,-1,false},{"QueryHeadCount",2,4,128,-1,false},{"KeyValueHeadCount",2,4,132,-1,false},{"MaskType",2,4,136,-1,false}};
inline constexpr OperatorField DML_QUANTIZE_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"QuantizationType",2,4,8,-1,false},{"QuantizationTensorCount",2,4,12,-1,false},{"QuantizationTensors",0,1,16,2,false},{"OutputTensor",1,0,24,-1,false}};
inline constexpr OperatorField DML_DEQUANTIZE_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"QuantizationType",2,4,8,-1,false},{"QuantizationTensorCount",2,4,12,-1,false},{"QuantizationTensors",0,1,16,2,false},{"OutputTensor",1,0,24,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_ELU_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Alpha",2,7,16,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_CELU_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Alpha",2,7,16,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_HARDMAX_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_HARDMAX1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"AxisCount",2,4,16,-1,false},{"Axes",2,8,24,2,false}};
inline constexpr OperatorField DML_ACTIVATION_HARD_SIGMOID_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Alpha",2,7,16,-1,false},{"Beta",2,7,20,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_IDENTITY_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_LEAKY_RELU_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Alpha",2,7,16,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_LINEAR_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Alpha",2,7,16,-1,false},{"Beta",2,7,20,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_LOG_SOFTMAX_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_LOG_SOFTMAX1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"AxisCount",2,4,16,-1,false},{"Axes",2,8,24,2,false}};
inline constexpr OperatorField DML_ACTIVATION_PARAMETERIZED_RELU_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"SlopeTensor",0,0,8,-1,false},{"OutputTensor",1,0,16,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_PARAMETRIC_SOFTPLUS_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Alpha",2,7,16,-1,false},{"Beta",2,7,20,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_RELU_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_SCALED_ELU_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Alpha",2,7,16,-1,false},{"Gamma",2,7,20,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_SCALED_TANH_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Alpha",2,7,16,-1,false},{"Beta",2,7,20,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_SIGMOID_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_SOFTMAX_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_SOFTMAX1_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"AxisCount",2,4,16,-1,false},{"Axes",2,8,24,2,false}};
inline constexpr OperatorField DML_ACTIVATION_SOFTPLUS_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Steepness",2,7,16,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_SOFTSIGN_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_TANH_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_THRESHOLDED_RELU_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Alpha",2,7,16,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_SHRINK_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Bias",2,7,16,-1,false},{"Threshold",2,7,20,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_GELU_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_SWISH_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"SigmoidInputScale",2,7,16,-1,false}};
inline constexpr OperatorField DML_ACTIVATION_HARD_SWISH_OPERATOR_fields[]{{"InputTensor",0,0,0,-1,false},{"OutputTensor",1,0,8,-1,false},{"Alpha",2,7,16,-1,false},{"Beta",2,7,20,-1,false}};
inline constexpr OperatorSchema operators[]{
    {"DML_OPERATOR_ELEMENT_WISE_IDENTITY",DML_OPERATOR_ELEMENT_WISE_IDENTITY,DML_ELEMENT_WISE_IDENTITY_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_ABS",DML_OPERATOR_ELEMENT_WISE_ABS,DML_ELEMENT_WISE_ABS_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_ACOS",DML_OPERATOR_ELEMENT_WISE_ACOS,DML_ELEMENT_WISE_ACOS_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_ADD",DML_OPERATOR_ELEMENT_WISE_ADD,DML_ELEMENT_WISE_ADD_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_ASIN",DML_OPERATOR_ELEMENT_WISE_ASIN,DML_ELEMENT_WISE_ASIN_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_ATAN",DML_OPERATOR_ELEMENT_WISE_ATAN,DML_ELEMENT_WISE_ATAN_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_CEIL",DML_OPERATOR_ELEMENT_WISE_CEIL,DML_ELEMENT_WISE_CEIL_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_CLIP",DML_OPERATOR_ELEMENT_WISE_CLIP,DML_ELEMENT_WISE_CLIP_OPERATOR_fields,32},
    {"DML_OPERATOR_ELEMENT_WISE_CLIP1",DML_OPERATOR_ELEMENT_WISE_CLIP1,DML_ELEMENT_WISE_CLIP1_OPERATOR_fields,48},
    {"DML_OPERATOR_ELEMENT_WISE_CLIP_GRAD",DML_OPERATOR_ELEMENT_WISE_CLIP_GRAD,DML_ELEMENT_WISE_CLIP_GRAD_OPERATOR_fields,32},
    {"DML_OPERATOR_ELEMENT_WISE_CLIP_GRAD1",DML_OPERATOR_ELEMENT_WISE_CLIP_GRAD1,DML_ELEMENT_WISE_CLIP_GRAD1_OPERATOR_fields,48},
    {"DML_OPERATOR_ELEMENT_WISE_COS",DML_OPERATOR_ELEMENT_WISE_COS,DML_ELEMENT_WISE_COS_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_DIVIDE",DML_OPERATOR_ELEMENT_WISE_DIVIDE,DML_ELEMENT_WISE_DIVIDE_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_EXP",DML_OPERATOR_ELEMENT_WISE_EXP,DML_ELEMENT_WISE_EXP_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_FLOOR",DML_OPERATOR_ELEMENT_WISE_FLOOR,DML_ELEMENT_WISE_FLOOR_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_LOG",DML_OPERATOR_ELEMENT_WISE_LOG,DML_ELEMENT_WISE_LOG_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_LOGICAL_AND",DML_OPERATOR_ELEMENT_WISE_LOGICAL_AND,DML_ELEMENT_WISE_LOGICAL_AND_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_LOGICAL_EQUALS",DML_OPERATOR_ELEMENT_WISE_LOGICAL_EQUALS,DML_ELEMENT_WISE_LOGICAL_EQUALS_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_LOGICAL_GREATER_THAN",DML_OPERATOR_ELEMENT_WISE_LOGICAL_GREATER_THAN,DML_ELEMENT_WISE_LOGICAL_GREATER_THAN_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_LOGICAL_LESS_THAN",DML_OPERATOR_ELEMENT_WISE_LOGICAL_LESS_THAN,DML_ELEMENT_WISE_LOGICAL_LESS_THAN_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_LOGICAL_GREATER_THAN_OR_EQUAL",DML_OPERATOR_ELEMENT_WISE_LOGICAL_GREATER_THAN_OR_EQUAL,DML_ELEMENT_WISE_LOGICAL_GREATER_THAN_OR_EQUAL_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_LOGICAL_LESS_THAN_OR_EQUAL",DML_OPERATOR_ELEMENT_WISE_LOGICAL_LESS_THAN_OR_EQUAL,DML_ELEMENT_WISE_LOGICAL_LESS_THAN_OR_EQUAL_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_LOGICAL_NOT",DML_OPERATOR_ELEMENT_WISE_LOGICAL_NOT,DML_ELEMENT_WISE_LOGICAL_NOT_OPERATOR_fields,16},
    {"DML_OPERATOR_ELEMENT_WISE_LOGICAL_OR",DML_OPERATOR_ELEMENT_WISE_LOGICAL_OR,DML_ELEMENT_WISE_LOGICAL_OR_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_LOGICAL_XOR",DML_OPERATOR_ELEMENT_WISE_LOGICAL_XOR,DML_ELEMENT_WISE_LOGICAL_XOR_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_MAX",DML_OPERATOR_ELEMENT_WISE_MAX,DML_ELEMENT_WISE_MAX_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_MEAN",DML_OPERATOR_ELEMENT_WISE_MEAN,DML_ELEMENT_WISE_MEAN_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_MIN",DML_OPERATOR_ELEMENT_WISE_MIN,DML_ELEMENT_WISE_MIN_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_MULTIPLY",DML_OPERATOR_ELEMENT_WISE_MULTIPLY,DML_ELEMENT_WISE_MULTIPLY_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_POW",DML_OPERATOR_ELEMENT_WISE_POW,DML_ELEMENT_WISE_POW_OPERATOR_fields,32},
    {"DML_OPERATOR_ELEMENT_WISE_CONSTANT_POW",DML_OPERATOR_ELEMENT_WISE_CONSTANT_POW,DML_ELEMENT_WISE_CONSTANT_POW_OPERATOR_fields,32},
    {"DML_OPERATOR_ELEMENT_WISE_RECIP",DML_OPERATOR_ELEMENT_WISE_RECIP,DML_ELEMENT_WISE_RECIP_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_SIN",DML_OPERATOR_ELEMENT_WISE_SIN,DML_ELEMENT_WISE_SIN_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_SQRT",DML_OPERATOR_ELEMENT_WISE_SQRT,DML_ELEMENT_WISE_SQRT_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_DIFFERENCE_SQUARE",DML_OPERATOR_ELEMENT_WISE_DIFFERENCE_SQUARE,DML_ELEMENT_WISE_DIFFERENCE_SQUARE_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_ATAN_YX",DML_OPERATOR_ELEMENT_WISE_ATAN_YX,DML_ELEMENT_WISE_ATAN_YX_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_SUBTRACT",DML_OPERATOR_ELEMENT_WISE_SUBTRACT,DML_ELEMENT_WISE_SUBTRACT_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_TAN",DML_OPERATOR_ELEMENT_WISE_TAN,DML_ELEMENT_WISE_TAN_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_THRESHOLD",DML_OPERATOR_ELEMENT_WISE_THRESHOLD,DML_ELEMENT_WISE_THRESHOLD_OPERATOR_fields,32},
    {"DML_OPERATOR_ELEMENT_WISE_QUANTIZE_LINEAR",DML_OPERATOR_ELEMENT_WISE_QUANTIZE_LINEAR,DML_ELEMENT_WISE_QUANTIZE_LINEAR_OPERATOR_fields,32},
    {"DML_OPERATOR_ELEMENT_WISE_DEQUANTIZE_LINEAR",DML_OPERATOR_ELEMENT_WISE_DEQUANTIZE_LINEAR,DML_ELEMENT_WISE_DEQUANTIZE_LINEAR_OPERATOR_fields,32},
    {"DML_OPERATOR_CONVOLUTION",DML_OPERATOR_CONVOLUTION,DML_CONVOLUTION_OPERATOR_fields,104},
    {"DML_OPERATOR_GEMM",DML_OPERATOR_GEMM,DML_GEMM_OPERATOR_fields,56},
    {"DML_OPERATOR_REDUCE",DML_OPERATOR_REDUCE,DML_REDUCE_OPERATOR_fields,40},
    {"DML_OPERATOR_ARGMIN",DML_OPERATOR_ARGMIN,DML_ARGMIN_OPERATOR_fields,40},
    {"DML_OPERATOR_ARGMAX",DML_OPERATOR_ARGMAX,DML_ARGMAX_OPERATOR_fields,40},
    {"DML_OPERATOR_AVERAGE_POOLING",DML_OPERATOR_AVERAGE_POOLING,DML_AVERAGE_POOLING_OPERATOR_fields,64},
    {"DML_OPERATOR_AVERAGE_POOLING1",DML_OPERATOR_AVERAGE_POOLING1,DML_AVERAGE_POOLING1_OPERATOR_fields,72},
    {"DML_OPERATOR_LP_POOLING",DML_OPERATOR_LP_POOLING,DML_LP_POOLING_OPERATOR_fields,64},
    {"DML_OPERATOR_LP_POOLING1",DML_OPERATOR_LP_POOLING1,DML_LP_POOLING1_OPERATOR_fields,72},
    {"DML_OPERATOR_MAX_POOLING",DML_OPERATOR_MAX_POOLING,DML_MAX_POOLING_OPERATOR_fields,56},
    {"DML_OPERATOR_MAX_POOLING1",DML_OPERATOR_MAX_POOLING1,DML_MAX_POOLING1_OPERATOR_fields,64},
    {"DML_OPERATOR_ROI_POOLING",DML_OPERATOR_ROI_POOLING,DML_ROI_POOLING_OPERATOR_fields,40},
    {"DML_OPERATOR_SLICE",DML_OPERATOR_SLICE,DML_SLICE_OPERATOR_fields,48},
    {"DML_OPERATOR_CAST",DML_OPERATOR_CAST,DML_CAST_OPERATOR_fields,16},
    {"DML_OPERATOR_SPLIT",DML_OPERATOR_SPLIT,DML_SPLIT_OPERATOR_fields,32},
    {"DML_OPERATOR_JOIN",DML_OPERATOR_JOIN,DML_JOIN_OPERATOR_fields,32},
    {"DML_OPERATOR_PADDING",DML_OPERATOR_PADDING,DML_PADDING_OPERATOR_fields,48},
    {"DML_OPERATOR_PADDING1",DML_OPERATOR_PADDING1,DML_PADDING1_OPERATOR_fields,56},
    {"DML_OPERATOR_VALUE_SCALE_2D",DML_OPERATOR_VALUE_SCALE_2D,DML_VALUE_SCALE_2D_OPERATOR_fields,32},
    {"DML_OPERATOR_UPSAMPLE_2D",DML_OPERATOR_UPSAMPLE_2D,DML_UPSAMPLE_2D_OPERATOR_fields,32},
    {"DML_OPERATOR_GATHER",DML_OPERATOR_GATHER,DML_GATHER_OPERATOR_fields,32},
    {"DML_OPERATOR_SPACE_TO_DEPTH",DML_OPERATOR_SPACE_TO_DEPTH,DML_SPACE_TO_DEPTH_OPERATOR_fields,24},
    {"DML_OPERATOR_DEPTH_TO_SPACE",DML_OPERATOR_DEPTH_TO_SPACE,DML_DEPTH_TO_SPACE_OPERATOR_fields,24},
    {"DML_OPERATOR_TILE",DML_OPERATOR_TILE,DML_TILE_OPERATOR_fields,32},
    {"DML_OPERATOR_TOP_K",DML_OPERATOR_TOP_K,DML_TOP_K_OPERATOR_fields,32},
    {"DML_OPERATOR_BATCH_NORMALIZATION",DML_OPERATOR_BATCH_NORMALIZATION,DML_BATCH_NORMALIZATION_OPERATOR_fields,64},
    {"DML_OPERATOR_BATCH_NORMALIZATION_GRAD",DML_OPERATOR_BATCH_NORMALIZATION_GRAD,DML_BATCH_NORMALIZATION_GRAD_OPERATOR_fields,72},
    {"DML_OPERATOR_BATCH_NORMALIZATION_TRAINING_GRAD",DML_OPERATOR_BATCH_NORMALIZATION_TRAINING_GRAD,DML_BATCH_NORMALIZATION_TRAINING_GRAD_OPERATOR_fields,72},
    {"DML_OPERATOR_MEAN_VARIANCE_NORMALIZATION",DML_OPERATOR_MEAN_VARIANCE_NORMALIZATION,DML_MEAN_VARIANCE_NORMALIZATION_OPERATOR_fields,56},
    {"DML_OPERATOR_LOCAL_RESPONSE_NORMALIZATION",DML_OPERATOR_LOCAL_RESPONSE_NORMALIZATION,DML_LOCAL_RESPONSE_NORMALIZATION_OPERATOR_fields,40},
    {"DML_OPERATOR_LOCAL_RESPONSE_NORMALIZATION_GRAD",DML_OPERATOR_LOCAL_RESPONSE_NORMALIZATION_GRAD,DML_LOCAL_RESPONSE_NORMALIZATION_GRAD_OPERATOR_fields,48},
    {"DML_OPERATOR_LP_NORMALIZATION",DML_OPERATOR_LP_NORMALIZATION,DML_LP_NORMALIZATION_OPERATOR_fields,32},
    {"DML_OPERATOR_RNN",DML_OPERATOR_RNN,DML_RNN_OPERATOR_fields,88},
    {"DML_OPERATOR_LSTM",DML_OPERATOR_LSTM,DML_LSTM_OPERATOR_fields,120},
    {"DML_OPERATOR_GRU",DML_OPERATOR_GRU,DML_GRU_OPERATOR_fields,88},
    {"DML_OPERATOR_ELEMENT_WISE_SIGN",DML_OPERATOR_ELEMENT_WISE_SIGN,DML_ELEMENT_WISE_SIGN_OPERATOR_fields,16},
    {"DML_OPERATOR_ELEMENT_WISE_IS_NAN",DML_OPERATOR_ELEMENT_WISE_IS_NAN,DML_ELEMENT_WISE_IS_NAN_OPERATOR_fields,16},
    {"DML_OPERATOR_ELEMENT_WISE_NEGATE",DML_OPERATOR_ELEMENT_WISE_NEGATE,DML_ELEMENT_WISE_NEGATE_OPERATOR_fields,16},
    {"DML_OPERATOR_ELEMENT_WISE_ERF",DML_OPERATOR_ELEMENT_WISE_ERF,DML_ELEMENT_WISE_ERF_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_SINH",DML_OPERATOR_ELEMENT_WISE_SINH,DML_ELEMENT_WISE_SINH_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_COSH",DML_OPERATOR_ELEMENT_WISE_COSH,DML_ELEMENT_WISE_COSH_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_TANH",DML_OPERATOR_ELEMENT_WISE_TANH,DML_ELEMENT_WISE_TANH_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_ASINH",DML_OPERATOR_ELEMENT_WISE_ASINH,DML_ELEMENT_WISE_ASINH_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_ACOSH",DML_OPERATOR_ELEMENT_WISE_ACOSH,DML_ELEMENT_WISE_ACOSH_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_ATANH",DML_OPERATOR_ELEMENT_WISE_ATANH,DML_ELEMENT_WISE_ATANH_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_IF",DML_OPERATOR_ELEMENT_WISE_IF,DML_ELEMENT_WISE_IF_OPERATOR_fields,32},
    {"DML_OPERATOR_ELEMENT_WISE_ADD1",DML_OPERATOR_ELEMENT_WISE_ADD1,DML_ELEMENT_WISE_ADD1_OPERATOR_fields,32},
    {"DML_OPERATOR_MAX_UNPOOLING",DML_OPERATOR_MAX_UNPOOLING,DML_MAX_UNPOOLING_OPERATOR_fields,24},
    {"DML_OPERATOR_DIAGONAL_MATRIX",DML_OPERATOR_DIAGONAL_MATRIX,DML_DIAGONAL_MATRIX_OPERATOR_fields,16},
    {"DML_OPERATOR_SCATTER",DML_OPERATOR_SCATTER,DML_SCATTER_OPERATOR_fields,40},
    {"DML_OPERATOR_ONE_HOT",DML_OPERATOR_ONE_HOT,DML_ONE_HOT_OPERATOR_fields,32},
    {"DML_OPERATOR_RESAMPLE",DML_OPERATOR_RESAMPLE,DML_RESAMPLE_OPERATOR_fields,32},
    {"DML_OPERATOR_ELEMENT_WISE_BIT_SHIFT_LEFT",DML_OPERATOR_ELEMENT_WISE_BIT_SHIFT_LEFT,DML_ELEMENT_WISE_BIT_SHIFT_LEFT_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_BIT_SHIFT_RIGHT",DML_OPERATOR_ELEMENT_WISE_BIT_SHIFT_RIGHT,DML_ELEMENT_WISE_BIT_SHIFT_RIGHT_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_ROUND",DML_OPERATOR_ELEMENT_WISE_ROUND,DML_ELEMENT_WISE_ROUND_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_IS_INFINITY",DML_OPERATOR_ELEMENT_WISE_IS_INFINITY,DML_ELEMENT_WISE_IS_INFINITY_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_MODULUS_TRUNCATE",DML_OPERATOR_ELEMENT_WISE_MODULUS_TRUNCATE,DML_ELEMENT_WISE_MODULUS_TRUNCATE_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_MODULUS_FLOOR",DML_OPERATOR_ELEMENT_WISE_MODULUS_FLOOR,DML_ELEMENT_WISE_MODULUS_FLOOR_OPERATOR_fields,24},
    {"DML_OPERATOR_FILL_VALUE_CONSTANT",DML_OPERATOR_FILL_VALUE_CONSTANT,DML_FILL_VALUE_CONSTANT_OPERATOR_fields,24},
    {"DML_OPERATOR_FILL_VALUE_SEQUENCE",DML_OPERATOR_FILL_VALUE_SEQUENCE,DML_FILL_VALUE_SEQUENCE_OPERATOR_fields,32},
    {"DML_OPERATOR_CUMULATIVE_SUMMATION",DML_OPERATOR_CUMULATIVE_SUMMATION,DML_CUMULATIVE_SUMMATION_OPERATOR_fields,32},
    {"DML_OPERATOR_CUMULATIVE_PRODUCT",DML_OPERATOR_CUMULATIVE_PRODUCT,DML_CUMULATIVE_PRODUCT_OPERATOR_fields,32},
    {"DML_OPERATOR_REVERSE_SUBSEQUENCES",DML_OPERATOR_REVERSE_SUBSEQUENCES,DML_REVERSE_SUBSEQUENCES_OPERATOR_fields,32},
    {"DML_OPERATOR_GATHER_ELEMENTS",DML_OPERATOR_GATHER_ELEMENTS,DML_GATHER_ELEMENTS_OPERATOR_fields,32},
    {"DML_OPERATOR_GATHER_ND",DML_OPERATOR_GATHER_ND,DML_GATHER_ND_OPERATOR_fields,32},
    {"DML_OPERATOR_SCATTER_ND",DML_OPERATOR_SCATTER_ND,DML_SCATTER_ND_OPERATOR_fields,40},
    {"DML_OPERATOR_MAX_POOLING2",DML_OPERATOR_MAX_POOLING2,DML_MAX_POOLING2_OPERATOR_fields,72},
    {"DML_OPERATOR_SLICE1",DML_OPERATOR_SLICE1,DML_SLICE1_OPERATOR_fields,48},
    {"DML_OPERATOR_TOP_K1",DML_OPERATOR_TOP_K1,DML_TOP_K1_OPERATOR_fields,40},
    {"DML_OPERATOR_DEPTH_TO_SPACE1",DML_OPERATOR_DEPTH_TO_SPACE1,DML_DEPTH_TO_SPACE1_OPERATOR_fields,24},
    {"DML_OPERATOR_SPACE_TO_DEPTH1",DML_OPERATOR_SPACE_TO_DEPTH1,DML_SPACE_TO_DEPTH1_OPERATOR_fields,24},
    {"DML_OPERATOR_MEAN_VARIANCE_NORMALIZATION1",DML_OPERATOR_MEAN_VARIANCE_NORMALIZATION1,DML_MEAN_VARIANCE_NORMALIZATION1_OPERATOR_fields,64},
    {"DML_OPERATOR_RESAMPLE1",DML_OPERATOR_RESAMPLE1,DML_RESAMPLE1_OPERATOR_fields,48},
    {"DML_OPERATOR_MATRIX_MULTIPLY_INTEGER",DML_OPERATOR_MATRIX_MULTIPLY_INTEGER,DML_MATRIX_MULTIPLY_INTEGER_OPERATOR_fields,40},
    {"DML_OPERATOR_QUANTIZED_LINEAR_MATRIX_MULTIPLY",DML_OPERATOR_QUANTIZED_LINEAR_MATRIX_MULTIPLY,DML_QUANTIZED_LINEAR_MATRIX_MULTIPLY_OPERATOR_fields,72},
    {"DML_OPERATOR_CONVOLUTION_INTEGER",DML_OPERATOR_CONVOLUTION_INTEGER,DML_CONVOLUTION_INTEGER_OPERATOR_fields,88},
    {"DML_OPERATOR_QUANTIZED_LINEAR_CONVOLUTION",DML_OPERATOR_QUANTIZED_LINEAR_CONVOLUTION,DML_QUANTIZED_LINEAR_CONVOLUTION_OPERATOR_fields,128},
    {"DML_OPERATOR_ELEMENT_WISE_BIT_AND",DML_OPERATOR_ELEMENT_WISE_BIT_AND,DML_ELEMENT_WISE_BIT_AND_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_BIT_OR",DML_OPERATOR_ELEMENT_WISE_BIT_OR,DML_ELEMENT_WISE_BIT_OR_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_BIT_XOR",DML_OPERATOR_ELEMENT_WISE_BIT_XOR,DML_ELEMENT_WISE_BIT_XOR_OPERATOR_fields,24},
    {"DML_OPERATOR_ELEMENT_WISE_BIT_NOT",DML_OPERATOR_ELEMENT_WISE_BIT_NOT,DML_ELEMENT_WISE_BIT_NOT_OPERATOR_fields,16},
    {"DML_OPERATOR_ELEMENT_WISE_BIT_COUNT",DML_OPERATOR_ELEMENT_WISE_BIT_COUNT,DML_ELEMENT_WISE_BIT_COUNT_OPERATOR_fields,16},
    {"DML_OPERATOR_ACTIVATION_RELU_GRAD",DML_OPERATOR_ACTIVATION_RELU_GRAD,DML_ACTIVATION_RELU_GRAD_OPERATOR_fields,24},
    {"DML_OPERATOR_AVERAGE_POOLING_GRAD",DML_OPERATOR_AVERAGE_POOLING_GRAD,DML_AVERAGE_POOLING_GRAD_OPERATOR_fields,64},
    {"DML_OPERATOR_MAX_POOLING_GRAD",DML_OPERATOR_MAX_POOLING_GRAD,DML_MAX_POOLING_GRAD_OPERATOR_fields,72},
    {"DML_OPERATOR_RANDOM_GENERATOR",DML_OPERATOR_RANDOM_GENERATOR,DML_RANDOM_GENERATOR_OPERATOR_fields,32},
    {"DML_OPERATOR_NONZERO_COORDINATES",DML_OPERATOR_NONZERO_COORDINATES,DML_NONZERO_COORDINATES_OPERATOR_fields,24},
    {"DML_OPERATOR_RESAMPLE_GRAD",DML_OPERATOR_RESAMPLE_GRAD,DML_RESAMPLE_GRAD_OPERATOR_fields,48},
    {"DML_OPERATOR_SLICE_GRAD",DML_OPERATOR_SLICE_GRAD,DML_SLICE_GRAD_OPERATOR_fields,48},
    {"DML_OPERATOR_ADAM_OPTIMIZER",DML_OPERATOR_ADAM_OPTIMIZER,DML_ADAM_OPTIMIZER_OPERATOR_fields,80},
    {"DML_OPERATOR_ROI_ALIGN",DML_OPERATOR_ROI_ALIGN,DML_ROI_ALIGN_OPERATOR_fields,64},
    {"DML_OPERATOR_ROI_ALIGN1",DML_OPERATOR_ROI_ALIGN1,DML_ROI_ALIGN1_OPERATOR_fields,72},
    {"DML_OPERATOR_GATHER_ND1",DML_OPERATOR_GATHER_ND1,DML_GATHER_ND1_OPERATOR_fields,40},
    {"DML_OPERATOR_DYNAMIC_QUANTIZE_LINEAR",DML_OPERATOR_DYNAMIC_QUANTIZE_LINEAR,DML_DYNAMIC_QUANTIZE_LINEAR_OPERATOR_fields,32},
    {"DML_OPERATOR_ELEMENT_WISE_QUANTIZED_LINEAR_ADD",DML_OPERATOR_ELEMENT_WISE_QUANTIZED_LINEAR_ADD,DML_ELEMENT_WISE_QUANTIZED_LINEAR_ADD_OPERATOR_fields,72},
    {"DML_OPERATOR_ROI_ALIGN_GRAD",DML_OPERATOR_ROI_ALIGN_GRAD,DML_ROI_ALIGN_GRAD_OPERATOR_fields,88},
    {"DML_OPERATOR_BATCH_NORMALIZATION_TRAINING",DML_OPERATOR_BATCH_NORMALIZATION_TRAINING,DML_BATCH_NORMALIZATION_TRAINING_OPERATOR_fields,72},
    {"DML_OPERATOR_RESAMPLE2",DML_OPERATOR_RESAMPLE2,DML_RESAMPLE2_OPERATOR_fields,56},
    {"DML_OPERATOR_RESAMPLE_GRAD1",DML_OPERATOR_RESAMPLE_GRAD1,DML_RESAMPLE_GRAD1_OPERATOR_fields,56},
    {"DML_OPERATOR_DIAGONAL_MATRIX1",DML_OPERATOR_DIAGONAL_MATRIX1,DML_DIAGONAL_MATRIX1_OPERATOR_fields,40},
    {"DML_OPERATOR_MULTIHEAD_ATTENTION",DML_OPERATOR_MULTIHEAD_ATTENTION,DML_MULTIHEAD_ATTENTION_OPERATOR_fields,128},
    {"DML_OPERATOR_QUANTIZED_LINEAR_AVERAGE_POOLING",DML_OPERATOR_QUANTIZED_LINEAR_AVERAGE_POOLING,DML_QUANTIZED_LINEAR_AVERAGE_POOLING_OPERATOR_fields,104},
    {"DML_OPERATOR_MATRIX_MULTIPLY_INTEGER_TO_FLOAT",DML_OPERATOR_MATRIX_MULTIPLY_INTEGER_TO_FLOAT,DML_MATRIX_MULTIPLY_INTEGER_TO_FLOAT_OPERATOR_fields,64},
    {"DML_OPERATOR_MEAN_VARIANCE_NORMALIZATION2",DML_OPERATOR_MEAN_VARIANCE_NORMALIZATION2,DML_MEAN_VARIANCE_NORMALIZATION2_OPERATOR_fields,72},
    {"DML_OPERATOR_MULTIHEAD_ATTENTION1",DML_OPERATOR_MULTIHEAD_ATTENTION1,DML_MULTIHEAD_ATTENTION1_OPERATOR_fields,144},
    {"DML_OPERATOR_QUANTIZE",DML_OPERATOR_QUANTIZE,DML_QUANTIZE_OPERATOR_fields,32},
    {"DML_OPERATOR_DEQUANTIZE",DML_OPERATOR_DEQUANTIZE,DML_DEQUANTIZE_OPERATOR_fields,32},
    {"DML_OPERATOR_ACTIVATION_ELU",DML_OPERATOR_ACTIVATION_ELU,DML_ACTIVATION_ELU_OPERATOR_fields,24},
    {"DML_OPERATOR_ACTIVATION_CELU",DML_OPERATOR_ACTIVATION_CELU,DML_ACTIVATION_CELU_OPERATOR_fields,24},
    {"DML_OPERATOR_ACTIVATION_HARDMAX",DML_OPERATOR_ACTIVATION_HARDMAX,DML_ACTIVATION_HARDMAX_OPERATOR_fields,16},
    {"DML_OPERATOR_ACTIVATION_HARDMAX1",DML_OPERATOR_ACTIVATION_HARDMAX1,DML_ACTIVATION_HARDMAX1_OPERATOR_fields,32},
    {"DML_OPERATOR_ACTIVATION_HARD_SIGMOID",DML_OPERATOR_ACTIVATION_HARD_SIGMOID,DML_ACTIVATION_HARD_SIGMOID_OPERATOR_fields,24},
    {"DML_OPERATOR_ACTIVATION_IDENTITY",DML_OPERATOR_ACTIVATION_IDENTITY,DML_ACTIVATION_IDENTITY_OPERATOR_fields,16},
    {"DML_OPERATOR_ACTIVATION_LEAKY_RELU",DML_OPERATOR_ACTIVATION_LEAKY_RELU,DML_ACTIVATION_LEAKY_RELU_OPERATOR_fields,24},
    {"DML_OPERATOR_ACTIVATION_LINEAR",DML_OPERATOR_ACTIVATION_LINEAR,DML_ACTIVATION_LINEAR_OPERATOR_fields,24},
    {"DML_OPERATOR_ACTIVATION_LOG_SOFTMAX",DML_OPERATOR_ACTIVATION_LOG_SOFTMAX,DML_ACTIVATION_LOG_SOFTMAX_OPERATOR_fields,16},
    {"DML_OPERATOR_ACTIVATION_LOG_SOFTMAX1",DML_OPERATOR_ACTIVATION_LOG_SOFTMAX1,DML_ACTIVATION_LOG_SOFTMAX1_OPERATOR_fields,32},
    {"DML_OPERATOR_ACTIVATION_PARAMETERIZED_RELU",DML_OPERATOR_ACTIVATION_PARAMETERIZED_RELU,DML_ACTIVATION_PARAMETERIZED_RELU_OPERATOR_fields,24},
    {"DML_OPERATOR_ACTIVATION_PARAMETRIC_SOFTPLUS",DML_OPERATOR_ACTIVATION_PARAMETRIC_SOFTPLUS,DML_ACTIVATION_PARAMETRIC_SOFTPLUS_OPERATOR_fields,24},
    {"DML_OPERATOR_ACTIVATION_RELU",DML_OPERATOR_ACTIVATION_RELU,DML_ACTIVATION_RELU_OPERATOR_fields,16},
    {"DML_OPERATOR_ACTIVATION_SCALED_ELU",DML_OPERATOR_ACTIVATION_SCALED_ELU,DML_ACTIVATION_SCALED_ELU_OPERATOR_fields,24},
    {"DML_OPERATOR_ACTIVATION_SCALED_TANH",DML_OPERATOR_ACTIVATION_SCALED_TANH,DML_ACTIVATION_SCALED_TANH_OPERATOR_fields,24},
    {"DML_OPERATOR_ACTIVATION_SIGMOID",DML_OPERATOR_ACTIVATION_SIGMOID,DML_ACTIVATION_SIGMOID_OPERATOR_fields,16},
    {"DML_OPERATOR_ACTIVATION_SOFTMAX",DML_OPERATOR_ACTIVATION_SOFTMAX,DML_ACTIVATION_SOFTMAX_OPERATOR_fields,16},
    {"DML_OPERATOR_ACTIVATION_SOFTMAX1",DML_OPERATOR_ACTIVATION_SOFTMAX1,DML_ACTIVATION_SOFTMAX1_OPERATOR_fields,32},
    {"DML_OPERATOR_ACTIVATION_SOFTPLUS",DML_OPERATOR_ACTIVATION_SOFTPLUS,DML_ACTIVATION_SOFTPLUS_OPERATOR_fields,24},
    {"DML_OPERATOR_ACTIVATION_SOFTSIGN",DML_OPERATOR_ACTIVATION_SOFTSIGN,DML_ACTIVATION_SOFTSIGN_OPERATOR_fields,16},
    {"DML_OPERATOR_ACTIVATION_TANH",DML_OPERATOR_ACTIVATION_TANH,DML_ACTIVATION_TANH_OPERATOR_fields,16},
    {"DML_OPERATOR_ACTIVATION_THRESHOLDED_RELU",DML_OPERATOR_ACTIVATION_THRESHOLDED_RELU,DML_ACTIVATION_THRESHOLDED_RELU_OPERATOR_fields,24},
    {"DML_OPERATOR_ACTIVATION_SHRINK",DML_OPERATOR_ACTIVATION_SHRINK,DML_ACTIVATION_SHRINK_OPERATOR_fields,24},
    {"DML_OPERATOR_ACTIVATION_GELU",DML_OPERATOR_ACTIVATION_GELU,DML_ACTIVATION_GELU_OPERATOR_fields,16},
    {"DML_OPERATOR_ACTIVATION_SWISH",DML_OPERATOR_ACTIVATION_SWISH,DML_ACTIVATION_SWISH_OPERATOR_fields,24},
    {"DML_OPERATOR_ACTIVATION_HARD_SWISH",DML_OPERATOR_ACTIVATION_HARD_SWISH,DML_ACTIVATION_HARD_SWISH_OPERATOR_fields,24},
};
constexpr const OperatorSchema* find_operator(std::uint32_t type) {
    for (const auto& item : operators) if (item.type == type) return &item;
    return nullptr;
}
constexpr const Interface* find_interface(std::string_view name) {
    for (const auto& item : interfaces) if (item.name == name) return &item;
    return nullptr;
}
}
