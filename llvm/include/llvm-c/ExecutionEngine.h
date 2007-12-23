/*===-- llvm-c/ExecutionEngine.h - ExecutionEngine Lib C Iface --*- C++ -*-===*\
|*                                                                            *|
|*                     The LLVM Compiler Infrastructure                       *|
|*                                                                            *|
|* This file was developed by Gordon Henriksen and is distributed under the   *|
|* University of Illinois Open Source License. See LICENSE.TXT for details.   *|
|*                                                                            *|
|*===----------------------------------------------------------------------===*|
|*                                                                            *|
|* This header declares the C interface to libLLVMExecutionEngine.o, which    *|
|* implements various analyses of the LLVM IR.                                *|
|*                                                                            *|
|* Many exotic languages can interoperate with C code but have a harder time  *|
|* with C++ due to name mangling. So in addition to C, this interface enables *|
|* tools written in such languages.                                           *|
|*                                                                            *|
\*===----------------------------------------------------------------------===*/

#ifndef LLVM_C_EXECUTIONENGINE_H
#define LLVM_C_EXECUTIONENGINE_H

#include "llvm-c/Core.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct LLVMOpaqueGenericValue *LLVMGenericValueRef;
typedef struct LLVMOpaqueExecutionEngine *LLVMExecutionEngineRef;

/*===-- Operations on generic values --------------------------------------===*/

LLVMGenericValueRef LLVMCreateGenericValueOfInt(LLVMTypeRef Ty,
                                                unsigned long long N,
                                                int IsSigned);

LLVMGenericValueRef LLVMCreateGenericValueOfPointer(void *P);

LLVMGenericValueRef LLVMCreateGenericValueOfFloat(LLVMTypeRef Ty, double N);

unsigned LLVMGenericValueIntWidth(LLVMGenericValueRef GenValRef);

unsigned long long LLVMGenericValueToInt(LLVMGenericValueRef GenVal,
                                         int IsSigned);

void *LLVMGenericValueToPointer(LLVMGenericValueRef GenVal);

double LLVMGenericValueToFloat(LLVMTypeRef TyRef, LLVMGenericValueRef GenVal);

void LLVMDisposeGenericValue(LLVMGenericValueRef GenVal);

/*===-- Operations on execution engines -----------------------------------===*/

int LLVMCreateExecutionEngine(LLVMExecutionEngineRef *OutEE,
                              LLVMModuleProviderRef MP,
                              char **OutError);

int LLVMCreateInterpreter(LLVMExecutionEngineRef *OutInterp,
                          LLVMModuleProviderRef MP,
                          char **OutError);

int LLVMCreateJITCompiler(LLVMExecutionEngineRef *OutJIT,
                          LLVMModuleProviderRef MP,
                          char **OutError);

void LLVMDisposeExecutionEngine(LLVMExecutionEngineRef EE);

void LLVMRunStaticConstructors(LLVMExecutionEngineRef EE);

void LLVMRunStaticDestructors(LLVMExecutionEngineRef EE);

int LLVMRunFunctionAsMain(LLVMExecutionEngineRef EE, LLVMValueRef F,
                          unsigned ArgC, const char * const *ArgV,
                          const char * const *EnvP);

LLVMGenericValueRef LLVMRunFunction(LLVMExecutionEngineRef EE, LLVMValueRef F,
                                    unsigned NumArgs,
                                    LLVMGenericValueRef *Args);

void LLVMFreeMachineCodeForFunction(LLVMExecutionEngineRef EE, LLVMValueRef F);

void LLVMAddModuleProvider(LLVMExecutionEngineRef EE, LLVMModuleProviderRef MP);

int LLVMRemoveModuleProvider(LLVMExecutionEngineRef EE,
                             LLVMModuleProviderRef MP,
                             LLVMModuleRef *OutMod, char **OutError);

int LLVMFindFunction(LLVMExecutionEngineRef EE, const char *Name,
                     LLVMValueRef *OutFn);

#ifdef __cplusplus
}

namespace llvm {
  class GenericValue;
  class ExecutionEngine;
  
  #define DEFINE_SIMPLE_CONVERSION_FUNCTIONS(ty, ref)   \
    inline ty *unwrap(ref P) {                          \
      return reinterpret_cast<ty*>(P);                  \
    }                                                   \
                                                        \
    inline ref wrap(const ty *P) {                      \
      return reinterpret_cast<ref>(const_cast<ty*>(P)); \
    }
  
  DEFINE_SIMPLE_CONVERSION_FUNCTIONS(GenericValue,    LLVMGenericValueRef   )
  DEFINE_SIMPLE_CONVERSION_FUNCTIONS(ExecutionEngine, LLVMExecutionEngineRef)
  
  #undef DEFINE_SIMPLE_CONVERSION_FUNCTIONS
}
  
#endif /* defined(__cplusplus) */

#endif
