// Copyright (c) 2016, Lawrence Livermore National Security, LLC.  
// Produced at the Lawrence Livermore National Laboratory.
//
// This file is part of Caliper.
// Written by David Boehme, boehme3@llnl.gov.
// LLNL-CODE-678900
// All rights reserved.
//
// For details, see https://github.com/scalability-llnl/Caliper.
// Please also see the LICENSE file for our additional BSD notice.
//
// Redistribution and use in source and binary forms, with or without modification, are
// permitted provided that the following conditions are met:
//
//  * Redistributions of source code must retain the above copyright notice, this list of
//    conditions and the disclaimer below.
//  * Redistributions in binary form must reproduce the above copyright notice, this list of
//    conditions and the disclaimer (as noted below) in the documentation and/or other materials
//    provided with the distribution.
//  * Neither the name of the LLNS/LLNL nor the names of its contributors may be used to endorse
//    or promote products derived from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
// LAWRENCE LIVERMORE NATIONAL SECURITY, LLC, THE U.S. DEPARTMENT OF ENERGY OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
// (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/// \file  VTune.cpp
/// \brief Caliper vtune connector service

#include "../CaliperService.h"

#include <filters/DefaultFilter.h>

#include "../common/ToolWrapper.h"

#include <ittnotify.h>

namespace cali
{
    
class VTuneWrapper : public ToolWrapper<VTuneWrapper, DefaultFilter> {
public:

    static __itt_domain* cali_domain;

    static void initialize() {
        cali_domain = __itt_domain_create("caliper");
    }

    static std::string service_name() {
        return "VTune service";
    }
    
    static void beginAction(Caliper* c, const Attribute& attr, const Variant& value) {
        std::string str = attr.name();
        
        if (attr.type() != CALI_TYPE_BOOL) {
            str += "=" + value.to_string();
        }
        
        __itt_string_handle* h = __itt_string_handle_create(str.c_str());            
        __itt_task_begin(cali_domain, __itt_null, __itt_null, h);
    }

    static void endAction(Caliper* c, const Attribute& attr, const Variant& value) {
        __itt_task_end(cali_domain);
    }
};

__itt_domain* cali::VTuneWrapper::cali_domain = 0;

CaliperService vtune_service { "vtune", &VTuneWrapper::setCallbacks };

}
