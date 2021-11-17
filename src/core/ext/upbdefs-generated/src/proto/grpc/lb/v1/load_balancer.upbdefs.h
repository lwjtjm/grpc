/* This file was generated by upbc (the upb compiler) from the input
 * file:
 *
 *     src/proto/grpc/lb/v1/load_balancer.proto
 *
 * Do not edit -- your changes will be discarded when the file is
 * regenerated. */

#ifndef SRC_PROTO_GRPC_LB_V1_LOAD_BALANCER_PROTO_UPBDEFS_H_
#define SRC_PROTO_GRPC_LB_V1_LOAD_BALANCER_PROTO_UPBDEFS_H_

#include "upb/def.h"
#include "upb/port_def.inc"
#ifdef __cplusplus
extern "C" {
#endif

#include "upb/def.h"

#include "upb/port_def.inc"

extern upb_def_init src_proto_grpc_lb_v1_load_balancer_proto_upbdefinit;

UPB_INLINE const upb_msgdef *grpc_lb_v1_LoadBalanceRequest_getmsgdef(upb_symtab *s) {
  _upb_symtab_loaddefinit(s, &src_proto_grpc_lb_v1_load_balancer_proto_upbdefinit);
  return upb_symtab_lookupmsg(s, "grpc.lb.v1.LoadBalanceRequest");
}

UPB_INLINE const upb_msgdef *grpc_lb_v1_InitialLoadBalanceRequest_getmsgdef(upb_symtab *s) {
  _upb_symtab_loaddefinit(s, &src_proto_grpc_lb_v1_load_balancer_proto_upbdefinit);
  return upb_symtab_lookupmsg(s, "grpc.lb.v1.InitialLoadBalanceRequest");
}

UPB_INLINE const upb_msgdef *grpc_lb_v1_ClientStatsPerToken_getmsgdef(upb_symtab *s) {
  _upb_symtab_loaddefinit(s, &src_proto_grpc_lb_v1_load_balancer_proto_upbdefinit);
  return upb_symtab_lookupmsg(s, "grpc.lb.v1.ClientStatsPerToken");
}

UPB_INLINE const upb_msgdef *grpc_lb_v1_ClientStats_getmsgdef(upb_symtab *s) {
  _upb_symtab_loaddefinit(s, &src_proto_grpc_lb_v1_load_balancer_proto_upbdefinit);
  return upb_symtab_lookupmsg(s, "grpc.lb.v1.ClientStats");
}

UPB_INLINE const upb_msgdef *grpc_lb_v1_LoadBalanceResponse_getmsgdef(upb_symtab *s) {
  _upb_symtab_loaddefinit(s, &src_proto_grpc_lb_v1_load_balancer_proto_upbdefinit);
  return upb_symtab_lookupmsg(s, "grpc.lb.v1.LoadBalanceResponse");
}

UPB_INLINE const upb_msgdef *grpc_lb_v1_FallbackResponse_getmsgdef(upb_symtab *s) {
  _upb_symtab_loaddefinit(s, &src_proto_grpc_lb_v1_load_balancer_proto_upbdefinit);
  return upb_symtab_lookupmsg(s, "grpc.lb.v1.FallbackResponse");
}

UPB_INLINE const upb_msgdef *grpc_lb_v1_InitialLoadBalanceResponse_getmsgdef(upb_symtab *s) {
  _upb_symtab_loaddefinit(s, &src_proto_grpc_lb_v1_load_balancer_proto_upbdefinit);
  return upb_symtab_lookupmsg(s, "grpc.lb.v1.InitialLoadBalanceResponse");
}

UPB_INLINE const upb_msgdef *grpc_lb_v1_ServerList_getmsgdef(upb_symtab *s) {
  _upb_symtab_loaddefinit(s, &src_proto_grpc_lb_v1_load_balancer_proto_upbdefinit);
  return upb_symtab_lookupmsg(s, "grpc.lb.v1.ServerList");
}

UPB_INLINE const upb_msgdef *grpc_lb_v1_Server_getmsgdef(upb_symtab *s) {
  _upb_symtab_loaddefinit(s, &src_proto_grpc_lb_v1_load_balancer_proto_upbdefinit);
  return upb_symtab_lookupmsg(s, "grpc.lb.v1.Server");
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#include "upb/port_undef.inc"

#endif  /* SRC_PROTO_GRPC_LB_V1_LOAD_BALANCER_PROTO_UPBDEFS_H_ */