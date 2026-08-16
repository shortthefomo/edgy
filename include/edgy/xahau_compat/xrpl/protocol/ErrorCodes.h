#pragma once

#include_next <xrpl/protocol/ErrorCodes.h>

namespace ripple {

inline constexpr auto RpcSrcActMissing = rpcSRC_ACT_MISSING;
inline constexpr auto RpcDstActMissing = rpcDST_ACT_MISSING;
inline constexpr auto RpcDstAmtMissing = rpcDST_AMT_MISSING;
inline constexpr auto RpcSrcActMalformed = rpcSRC_ACT_MALFORMED;
inline constexpr auto RpcDstActMalformed = rpcDST_ACT_MALFORMED;
inline constexpr auto RpcDstAmtMalformed = rpcDST_AMT_MALFORMED;
inline constexpr auto RpcSendmaxMalformed = rpcSENDMAX_MALFORMED;
inline constexpr auto RpcSrcCurMalformed = rpcSRC_CUR_MALFORMED;
inline constexpr auto RpcSrcIsrMalformed = rpcSRC_ISR_MALFORMED;
inline constexpr auto RpcSrcActNotFound = rpcSRC_ACT_NOT_FOUND;
inline constexpr auto RpcActNotFound = rpcACT_NOT_FOUND;
inline constexpr auto RpcNotSynced = rpcNOT_SYNCED;
inline constexpr auto RpcInvalidParams = rpcINVALID_PARAMS;
inline constexpr auto RpcNoPfRequest = rpcNO_PF_REQUEST;
inline constexpr auto RpcInternal = rpcINTERNAL;
inline constexpr auto RpcUnknownCommand = rpcUNKNOWN_COMMAND;
inline constexpr auto RpcNoCurrent = rpcNO_CURRENT;
inline constexpr auto RpcDomainMalformed = rpcINVALID_PARAMS;

}  // namespace ripple
