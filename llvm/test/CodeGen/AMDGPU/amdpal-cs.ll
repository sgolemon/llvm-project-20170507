; RUN: llc -mtriple=amdgcn--amdpal -verify-machineinstrs < %s | FileCheck -check-prefix=GCN -check-prefix=SI -enable-var-scope %s
; RUN: llc -mtriple=amdgcn--amdpal -mcpu=tonga -verify-machineinstrs < %s | FileCheck -check-prefix=GCN -check-prefix=VI -enable-var-scope %s
; RUN: llc -mtriple=amdgcn--amdpal -mcpu=gfx900 -verify-machineinstrs < %s | FileCheck -check-prefix=GCN -check-prefix=GFX9 -enable-var-scope %s

; amdpal compute shader: check for 0x2e12 (COMPUTE_PGM_RSRC1) in pal metadata
; GCN-LABEL: {{^}}cs_amdpal:
; GCN: .amdgpu_pal_metadata{{.*}}0x2e12,
define amdgpu_cs half @cs_amdpal(half %arg0) {
  %add = fadd half %arg0, 1.0
  ret half %add
}

