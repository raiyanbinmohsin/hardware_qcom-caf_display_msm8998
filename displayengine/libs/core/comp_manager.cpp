/*
* Copyright (c) 2014, The Linux Foundation. All rights reserved.
*
* Redistribution and use in source and binary forms, with or without modification, are permitted
* provided that the following conditions are met:
*    * Redistributions of source code must retain the above copyright notice, this list of
*      conditions and the following disclaimer.
*    * Redistributions in binary form must reproduce the above copyright notice, this list of
*      conditions and the following disclaimer in the documentation and/or other materials provided
*      with the distribution.
*    * Neither the name of The Linux Foundation nor the names of its contributors may be used to
*      endorse or promote products derived from this software without specific prior written
*      permission.
*
* THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
* NON-INFRINGEMENT ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
* FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
* OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
* STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
* OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

// SDE_LOG_TAG definition must precede debug.h include.
#define SDE_LOG_TAG kTagCore
#define SDE_MODULE_NAME "CompManager"
#include <utils/debug.h>

#include <dlfcn.h>
#include <utils/constants.h>

#include "comp_manager.h"

namespace sde {

CompManager::CompManager() : strategy_lib_(NULL), strategy_intf_(NULL), registered_displays_(0),
                             configured_displays_(0), safe_mode_(false) {
}

DisplayError CompManager::Init(const HWResourceInfo &hw_res_info) {
  SCOPE_LOCK(locker_);

  DisplayError error = kErrorNone;

  error = res_mgr_.Init(hw_res_info);
  if (UNLIKELY(error != kErrorNone)) {
    return error;
  }

  // Try to load strategy library & get handle to its interface.
  // Default to GPU only composition on failure.
  strategy_lib_ = ::dlopen(STRATEGY_LIBRARY_NAME, RTLD_NOW);
  if (UNLIKELY(!strategy_lib_)) {
    DLOGW("Unable to load = %s", STRATEGY_LIBRARY_NAME);
  } else {
    GetStrategyInterface get_strategy_intf = NULL;
    void **sym = reinterpret_cast<void **>(&get_strategy_intf);
    *sym = ::dlsym(strategy_lib_, GET_STRATEGY_INTERFACE_NAME);
    if (UNLIKELY(!get_strategy_intf)) {
      DLOGW("Unable to find symbol for %s", GET_STRATEGY_INTERFACE_NAME);
    } else if (UNLIKELY(get_strategy_intf(&strategy_intf_) != kErrorNone)) {
      DLOGW("Unable to get handle to strategy interface");
    }
  }

  if (UNLIKELY(!strategy_intf_)) {
    DLOGI("Using GPU only composition");
    if (strategy_lib_) {
      ::dlclose(strategy_lib_);
      strategy_lib_ = NULL;
    }
    strategy_intf_ = &strategy_default_;
  }

  return kErrorNone;
}

DisplayError CompManager::Deinit() {
  SCOPE_LOCK(locker_);

  if (strategy_lib_) {
    ::dlclose(strategy_lib_);
  }
  res_mgr_.Deinit();

  return kErrorNone;
}

DisplayError CompManager::RegisterDevice(DeviceType type, const HWDeviceAttributes &attributes,
                                         Handle *device) {
  SCOPE_LOCK(locker_);

  DisplayError error = kErrorNone;

  CompManagerDevice *comp_mgr_device = new CompManagerDevice();
  if (!comp_mgr_device) {
    return kErrorMemory;
  }

  error = res_mgr_.RegisterDevice(type, attributes, &comp_mgr_device->res_mgr_device);
  if (error != kErrorNone) {
    delete comp_mgr_device;
    return error;
  }
  SET_BIT(registered_displays_, type);
  comp_mgr_device->device_type = type;
  *device = comp_mgr_device;
  // New device has been added, so move the composition mode to safe mode until unless resources
  // for the added display is configured properly.
  safe_mode_ = true;

  return kErrorNone;
}

DisplayError CompManager::UnregisterDevice(Handle device) {
  SCOPE_LOCK(locker_);

  CompManagerDevice *comp_mgr_device = reinterpret_cast<CompManagerDevice *>(device);

  res_mgr_.UnregisterDevice(comp_mgr_device->res_mgr_device);
  CLEAR_BIT(registered_displays_, comp_mgr_device->device_type);
  CLEAR_BIT(configured_displays_, comp_mgr_device->device_type);
  delete comp_mgr_device;

  return kErrorNone;
}

void CompManager::PrepareStrategyConstraints(Handle device, HWLayers *hw_layers) {
  CompManagerDevice *comp_mgr_device = reinterpret_cast<CompManagerDevice *>(device);
  StrategyConstraints *constraints = &comp_mgr_device->constraints;

  constraints->safe_mode = safe_mode_;
  // If validation for the best available composition strategy with driver has failed, just
  // fallback to GPU composition.
  if (UNLIKELY(hw_layers->info.flags)) {
    constraints->safe_mode = true;
    return;
  }
}

DisplayError CompManager::Prepare(Handle device, HWLayers *hw_layers) {
  SCOPE_LOCK(locker_);

  CompManagerDevice *comp_mgr_device = reinterpret_cast<CompManagerDevice *>(device);
  Handle &res_mgr_device = comp_mgr_device->res_mgr_device;

  DisplayError error = kErrorNone;

  PrepareStrategyConstraints(device, hw_layers);

  // Select a composition strategy, and try to allocate resources for it.
  res_mgr_.Start(res_mgr_device);
  while (true) {
    error = strategy_intf_->GetNextStrategy(&comp_mgr_device->constraints, &hw_layers->info);
    if (UNLIKELY(error != kErrorNone)) {
      // Composition strategies exhausted. Resource Manager could not allocate resources even for
      // GPU composition. This will never happen.
      DLOGE("Unexpected failure. Composition strategies exhausted.");
      return error;
    }

    error = res_mgr_.Acquire(res_mgr_device, hw_layers);
    if (error != kErrorNone) {
      // Not enough resources, try next strategy.
      continue;
    } else {
      // Successfully selected and configured a composition strategy.
      break;
    }
  }
  res_mgr_.Stop(res_mgr_device);

  return kErrorNone;
}

void CompManager::PostPrepare(Handle device, HWLayers *hw_layers) {
  SCOPE_LOCK(locker_);
}

void CompManager::PostCommit(Handle device, HWLayers *hw_layers) {
  SCOPE_LOCK(locker_);

  CompManagerDevice *comp_mgr_device = reinterpret_cast<CompManagerDevice *>(device);
  SET_BIT(configured_displays_, comp_mgr_device->device_type);
  if (configured_displays_ == registered_displays_) {
      safe_mode_ = false;
  }

  res_mgr_.PostCommit(comp_mgr_device->res_mgr_device, hw_layers);
}

void CompManager::Purge(Handle device) {
  SCOPE_LOCK(locker_);

  CompManagerDevice *comp_mgr_device = reinterpret_cast<CompManagerDevice *>(device);

  res_mgr_.Purge(comp_mgr_device->res_mgr_device);
}

void CompManager::AppendDump(char *buffer, uint32_t length) {
  SCOPE_LOCK(locker_);
}

}  // namespace sde
