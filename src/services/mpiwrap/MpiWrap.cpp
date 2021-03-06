// Copyright (c) 2015, Lawrence Livermore National Security, LLC.  
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

///@file  MpiWrap.cpp
///@brief Caliper MPI service

#include "../CaliperService.h"

#include <Caliper.h>

#include <Log.h>
#include <RuntimeConfig.h>

using namespace cali;
using namespace std;

namespace cali
{
    Attribute mpifn_attr   { Attribute::invalid };
    Attribute mpirank_attr { Attribute::invalid };
    Attribute mpisize_attr { Attribute::invalid };

    bool      mpi_enabled  { false };

    string    mpi_whitelist_string;
    string    mpi_blacklist_string;
}

namespace
{

ConfigSet        config;

ConfigSet::Entry configdata[] = {
    { "whitelist", CALI_TYPE_STRING, "", 
      "List of MPI functions to instrument", 
      "Colon-separated list of MPI functions to instrument.\n"
      "If set, only whitelisted MPI functions will be instrumented.\n"
      "By default, all MPI functions are instrumented." 
    },
    { "blacklist", CALI_TYPE_STRING, "",
      "List of MPI functions to filter",
      "Colon-separated list of functions to blacklist." 
    },
    ConfigSet::Terminator
};

void mpi_register(Caliper* c)
{
    config = RuntimeConfig::init("mpi", configdata);

    mpifn_attr   = 
        c->create_attribute("mpi.function", CALI_TYPE_STRING);
    mpirank_attr = 
        c->create_attribute("mpi.rank", CALI_TYPE_INT, 
                            CALI_ATTR_SCOPE_PROCESS | CALI_ATTR_SKIP_EVENTS | CALI_ATTR_ASVALUE);
    mpisize_attr = 
        c->create_attribute("mpi.size", CALI_TYPE_INT, 
                            CALI_ATTR_SCOPE_PROCESS | CALI_ATTR_SKIP_EVENTS);

    mpi_enabled = true;

    mpi_whitelist_string = config.get("whitelist").to_string();
    mpi_blacklist_string = config.get("blacklist").to_string();

    Log(1).stream() << "Registered MPI service" << endl;
}

} // anonymous namespace 


namespace cali 
{
    CaliperService mpi_service = { "mpi", ::mpi_register };
} // namespace cali
