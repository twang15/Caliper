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

/// \file Caliper.cpp
/// Caliper main class
///

#include "caliper-config.h"

#include "Caliper.h"
#include "ContextBuffer.h"
#include "MetadataTree.h"
#include "MemoryPool.h"
#include "SigsafeRWLock.h"
#include "Snapshot.h"

#include <Services.h>

#include <ContextRecord.h>
#include <Node.h>
#include <Log.h>
#include <RecordMap.h>
#include <RuntimeConfig.h>

#include <signal.h>

#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <utility>

using namespace cali;
using namespace std;


namespace
{
    // --- helpers

    inline cali_context_scope_t 
    attr2caliscope(const Attribute& attr) {
        switch (attr.properties() & CALI_ATTR_SCOPE_MASK) {
        case CALI_ATTR_SCOPE_THREAD:
            return CALI_SCOPE_THREAD;
        case CALI_ATTR_SCOPE_PROCESS:
            return CALI_SCOPE_PROCESS;
        case CALI_ATTR_SCOPE_TASK:
            return CALI_SCOPE_TASK;    
        }

        // make thread scope the default
        return CALI_SCOPE_THREAD;
    }

    
    // --- Exit handler

    void
    exit_handler(void) {
        Caliper c = Caliper::instance();

        if (c)
            c.events().finish_evt(&c);

        Caliper::release();
    }

} // namespace


//
// Caliper Scope data
//

struct Caliper::Scope
{
    MemoryPool           mempool;
    ContextBuffer        statebuffer;
    
    cali_context_scope_t scope;

    Scope(cali_context_scope_t s)
        : scope(s) { }
};


//
// --- Caliper Global Data
//

struct Caliper::GlobalData
{
    // --- static data

    static volatile sig_atomic_t  s_siglock;
    static std::mutex             s_mutex;
    
    static const ConfigSet::Entry s_configdata[];

    static GlobalData*            sG;

    
    // --- data

    ConfigSet              config;

    ScopeCallbackFn        get_thread_scope_cb;
    ScopeCallbackFn        get_task_scope_cb;

    MetadataTree           tree;
    
    mutable SigsafeRWLock  attribute_lock;
    map<string, Node*>     attribute_nodes;

    Attribute              name_attr;
    Attribute              type_attr;
    Attribute              prop_attr;

    // Key attribute: one attribute stands in as key for all auto-merged attributes
    Attribute              key_attr;
    bool                   automerge;
    
    Events                 events;

    Scope*                 process_scope;
    Scope*                 default_thread_scope;
    Scope*                 default_task_scope;

    // --- constructor

    GlobalData()
        : config { RuntimeConfig::init("caliper", s_configdata) },
          get_thread_scope_cb { nullptr },
          get_task_scope_cb   { nullptr },
          name_attr { Attribute::invalid }, 
          type_attr { Attribute::invalid },  
          prop_attr { Attribute::invalid },
          key_attr  { Attribute::invalid },
          automerge { false },
          process_scope        { new Scope(CALI_SCOPE_PROCESS) },
          default_thread_scope { new Scope(CALI_SCOPE_THREAD)  },
          default_task_scope   { new Scope(CALI_SCOPE_TASK)    }
    {
        automerge = config.get("automerge").to_bool();

        const MetaAttributeIDs* m = tree.meta_attribute_ids();
        
        name_attr = Attribute::make_attribute(tree.node(m->name_attr_id), m);
        type_attr = Attribute::make_attribute(tree.node(m->type_attr_id), m);
        prop_attr = Attribute::make_attribute(tree.node(m->prop_attr_id), m);

        assert(name_attr != Attribute::invalid);
        assert(type_attr != Attribute::invalid);
        assert(prop_attr != Attribute::invalid);
    }

    ~GlobalData() {
        Log(1).stream() << "Finished" << endl;
        
        // prevent re-initialization
        s_siglock = 2;

	//freeing up context buffers
        delete process_scope;
        delete default_thread_scope; 
        delete default_task_scope;
    }

    const Attribute&
    get_key(const Attribute& attr) const {
        if (!automerge || attr.store_as_value() || !attr.is_autocombineable())
            return attr;

        return key_attr;
    }
};

// --- static member initialization

volatile sig_atomic_t  Caliper::GlobalData::s_siglock = 1;
mutex                  Caliper::GlobalData::s_mutex;

Caliper::GlobalData*   Caliper::GlobalData::sG = nullptr;

const ConfigSet::Entry Caliper::GlobalData::s_configdata[] = {
    // key, type, value, short description, long description
    { "automerge", CALI_TYPE_BOOL, "true",
      "Automatically merge attributes into a common context tree", 
      "Automatically merge attributes into a common context tree.\n"
      "Decreases the size of context records, but may increase\n"
      "the amount of metadata and reduce performance." 
    },
    ConfigSet::Terminator 
};


//
// Caliper class definition
//

Caliper::Scope*
Caliper::scope(cali_context_scope_t st) {
    switch (st) {
    case CALI_SCOPE_PROCESS:
        return mG->process_scope;
        
    case CALI_SCOPE_THREAD:
        if (!m_thread_scope)
            m_thread_scope =
                mG->get_thread_scope_cb ? mG->get_thread_scope_cb() : mG->default_thread_scope;

        return m_thread_scope;
        
    case CALI_SCOPE_TASK:
        if (!m_task_scope)
            m_task_scope =
                mG->get_task_scope_cb ? mG->get_task_scope_cb() : mG->default_task_scope;

        return m_task_scope;
    }

    return mG->process_scope;
}

Caliper::Scope*
Caliper::default_scope(cali_context_scope_t st)
{
    switch (st) {
    case CALI_SCOPE_THREAD:
        return mG->default_thread_scope;
        
    case CALI_SCOPE_TASK:
        return mG->default_task_scope;
        
    default:
        ;
    }

    return mG->process_scope;    
}

void 
Caliper::set_scope_callback(cali_context_scope_t scope, ScopeCallbackFn cb) {
    if (!mG)
        return;
    
    switch (scope) {
    case CALI_SCOPE_THREAD:
        if (mG->get_thread_scope_cb)
            Log(0).stream() 
                << "Caliper::set_context_callback(): error: callback already set" 
                << endl;
        mG->get_thread_scope_cb = cb;
        break;
    case CALI_SCOPE_TASK:
        if (mG->get_task_scope_cb)
            Log(0).stream() 
                << "Caliper::set_context_callback(): error: callback already set" 
                << endl;
        mG->get_task_scope_cb = cb;
        break;
    default:
        Log(0).stream() 
            << "Caliper::set_context_callback(): error: cannot set process callback" 
            << endl;
    }
}

Caliper::Scope*
Caliper::create_scope(cali_context_scope_t st)
{
    assert(mG != 0);
    
    Scope* s = new Scope(st);
    mG->events.create_scope_evt(this, st);

    return s;
}

void
Caliper::release_scope(Caliper::Scope* s)
{
    assert(mG != 0);
    
    mG->events.release_scope_evt(this, s->scope);
    // do NOT delete this because we still need the node data in the scope's memory pool
    // delete ctx;
}


// --- Attribute interface

Attribute 
Caliper::create_attribute(const std::string& name, cali_attr_type type, int prop)
{
    assert(mG != 0);
    
    // Add default SCOPE_THREAD property if no other is set
    if (!(prop & CALI_ATTR_SCOPE_PROCESS) && !(prop & CALI_ATTR_SCOPE_TASK))
        prop |= CALI_ATTR_SCOPE_THREAD;

    Node* node { nullptr };

    // Check if an attribute with this name already exists

    mG->attribute_lock.rlock();

    auto it = mG->attribute_nodes.find(name);
    if (it != mG->attribute_nodes.end())
        node = it->second;

    mG->attribute_lock.unlock();

    // Create attribute nodes

    if (!node) {
        assert(type >= 0 && type <= CALI_MAXTYPE);
        Node* type_node = mG->tree.type_node(type);
        assert(type_node);

        Attribute attr[2] { mG->prop_attr, mG->name_attr };
        Variant   data[2] { { prop }, { CALI_TYPE_STRING, name.c_str(), name.size() } };

        if (prop == CALI_ATTR_DEFAULT)
            node = mG->tree.get_path(1, &attr[1], &data[1], type_node, &mG->process_scope->mempool);
        else
            node = mG->tree.get_path(2, &attr[0], &data[0], type_node, &mG->process_scope->mempool);

        if (node) {
            // Check again if attribute already exists; might have been created by 
            // another thread in the meantime.
            // We've created some redundant nodes then, but that's fine
            mG->attribute_lock.wlock();

            auto it = mG->attribute_nodes.lower_bound(name);

            if (it == mG->attribute_nodes.end() || it->first != name)
                mG->attribute_nodes.emplace_hint(it, name, node);
            else
                node = it->second;

            mG->attribute_lock.unlock();
        }
    }

    // Create attribute object

    Attribute attr = Attribute::make_attribute(node, mG->tree.meta_attribute_ids());

    mG->events.create_attr_evt(this, attr);

    return attr;
}

Attribute
Caliper::get_attribute(const string& name) const
{
    assert(mG != 0);
    
    Node* node = nullptr;

    mG->attribute_lock.rlock();

    auto it = mG->attribute_nodes.find(name);

    if (it != mG->attribute_nodes.end())
        node = it->second;

    mG->attribute_lock.unlock();

    return Attribute::make_attribute(node, mG->tree.meta_attribute_ids());
}

Attribute 
Caliper::get_attribute(cali_id_t id) const
{
    assert(mG != 0);

    return Attribute::make_attribute(mG->tree.node(id), mG->tree.meta_attribute_ids());
}


// --- Snapshot interface

void
Caliper::pull_snapshot(int scopes, const Entry* trigger_info, Snapshot* sbuf)
{
    assert(mG != 0);

    // Save trigger info in snapshot buf

    if (trigger_info) {
        Snapshot::Sizes     sizes = { 0, 0, 0 };
        Snapshot::Addresses addresses = sbuf->addresses();

        if (trigger_info->node()) {
            // node entry
            addresses.node_entries[sizes.n_nodes++]  = trigger_info->node();
        } else {
            // as-value entry
            // todo: what to do with hidden attribute? - trigger info shouldn't be hidden though
            addresses.immediate_attr[sizes.n_attr++] = trigger_info->attribute();
            addresses.immediate_data[sizes.n_data++] = trigger_info->value();
        }

        sbuf->commit(sizes);
    }

    // Invoke callbacks and get contextbuffer data

    mG->events.snapshot(this, scopes, trigger_info, sbuf);

    for (cali_context_scope_t s : { CALI_SCOPE_TASK, CALI_SCOPE_THREAD, CALI_SCOPE_PROCESS })
        if (scopes & s)
            scope(s)->statebuffer.snapshot(sbuf);
}

void 
Caliper::push_snapshot(int scopes, const Entry* trigger_info)
{
    assert(mG != 0);

    Snapshot sbuf;

    pull_snapshot(scopes, trigger_info, &sbuf);

    mG->tree.write_new_nodes(mG->events.write_record);        

    mG->events.process_snapshot(this, trigger_info, &sbuf);
}


// --- Annotation interface

cali_err 
Caliper::begin(const Attribute& attr, const Variant& data)
{
    cali_err ret = CALI_EINV;

    if (!mG || attr == Attribute::invalid)
        return CALI_EINV;

    // invoke callbacks
    if (!attr.skip_events())
        mG->events.pre_begin_evt(this, attr);

    Scope* s = scope(attr2caliscope(attr));
    ContextBuffer* sb = &s->statebuffer;
    
    if (attr.store_as_value())
        ret = sb->set(attr, data);
    else
        ret = sb->set_node(mG->get_key(attr),
                           mG->tree.get_path(1, &attr, &data,
                                             sb->get_node(mG->get_key(attr)),
                                             &s->mempool));

    // invoke callbacks
    if (!attr.skip_events())
        mG->events.post_begin_evt(this, attr);

    return ret;
}

cali_err 
Caliper::end(const Attribute& attr)
{
    if (!mG || attr == Attribute::invalid)
        return CALI_EINV;

    cali_err ret = CALI_EINV;

    Scope* s = scope(attr2caliscope(attr));
    ContextBuffer* sb = &s->statebuffer;

    // invoke callbacks
    if (!attr.skip_events())
        mG->events.pre_end_evt(this, attr);

    if (attr.store_as_value())
        ret = sb->unset(attr);
    else {
        Node* node = sb->get_node(mG->get_key(attr));

        if (node) {
            node = mG->tree.remove_first_in_path(node, attr, &s->mempool);
                
            if (node == mG->tree.root())
                ret = sb->unset(mG->get_key(attr));
            else if (node)
                ret = sb->set_node(mG->get_key(attr), node);
        }

        if (!node)
            Log(0).stream() << "error: trying to end inactive attribute " << attr.name() << endl;
    }

    // invoke callbacks
    if (!attr.skip_events())
        mG->events.post_end_evt(this, attr);

    return ret;
}

cali_err 
Caliper::set(const Attribute& attr, const Variant& data)
{
    cali_err ret = CALI_EINV;

    if (!mG || attr == Attribute::invalid)
        return CALI_EINV;

    Scope* s = scope(attr2caliscope(attr));
    ContextBuffer* sb = &s->statebuffer;

    // invoke callbacks
    if (!attr.skip_events())
        mG->events.pre_set_evt(this, attr);

    if (attr.store_as_value())
        ret = sb->set(attr, data);
    else {
        Attribute key = mG->get_key(attr);
        
        ret = sb->set_node(key, mG->tree.replace_first_in_path(sb->get_node(key), attr, data, &s->mempool));
    }
    
    // invoke callbacks
    if (!attr.skip_events())
        mG->events.post_set_evt(this, attr);

    return ret;
}

cali_err 
Caliper::set_path(const Attribute& attr, size_t n, const Variant* data) {
    cali_err ret = CALI_EINV;

    if (!mG || attr == Attribute::invalid)
        return CALI_EINV;

    Scope* s = scope(attr2caliscope(attr));
    ContextBuffer* sb = &s->statebuffer;

    // invoke callbacks
    if (!attr.skip_events())
        mG->events.pre_set_evt(this, attr);

    if (attr.store_as_value()) {
        Log(0).stream() << "error: set_path() invoked with immediate-value attribute " << attr.name() << endl;
        ret = CALI_EINV;
    } else {
        Attribute key = mG->get_key(attr);
        
        ret = sb->set_node(key,
                           mG->tree.replace_all_in_path(sb->get_node(key), attr, n, data, &s->mempool));
    }
    
    // invoke callbacks
    if (!attr.skip_events())
        mG->events.post_set_evt(this, attr);

    return ret;
}

// --- Query

Entry
Caliper::get(const Attribute& attr) 
{
    Entry e {  Entry::empty };

    if (!mG || attr == Attribute::invalid)
        return Entry::empty;

    ContextBuffer* sb = &(scope(attr2caliscope(attr))->statebuffer);

    if (attr.store_as_value())
        return Entry(attr, sb->get(attr));
    else
        return Entry(mG->tree.find_node_with_attribute(attr, sb->get_node(mG->get_key(attr))));

    return e;
}

// --- Generic entry API

Entry
Caliper::make_entry(size_t n, const Attribute* attr, const Variant* value) 
{
    // what do we do with as-value attributes here?!
    return Entry(mG->tree.get_path(n, attr, value, nullptr, &mG->process_scope->mempool));
}

Entry 
Caliper::make_entry(const Attribute& attr, const Variant& value) 
{
    if (attr == Attribute::invalid)
        return Entry::empty;
    
    Entry entry { Entry::empty };

    if (attr.store_as_value())
        return Entry(attr, value);
    else
        return Entry(mG->tree.get_path(1, &attr, &value, nullptr, &(scope(attr2caliscope(attr))->mempool)));

    return entry;
}

// --- Events interface

Caliper::Events&
Caliper::events()
{
    return mG->events;
}

Variant
Caliper::exchange(const Attribute& attr, const Variant& data)
{
    return scope(attr2caliscope(attr))->statebuffer.exchange(attr, data);
}


//
// --- Caliper constructor & singleton API
//

Caliper::Caliper()
    : mG(0), m_thread_scope(0), m_task_scope(0)
{
    *this = Caliper::instance();
}

Caliper
Caliper::instance()
{
    if (GlobalData::s_siglock != 0) {
        if (GlobalData::s_siglock == 2)
            // Caliper had been initialized previously; we're past the static destructor
            return Caliper(0);

        if (atexit(::exit_handler) != 0)
            Log(0).stream() << "Unable to register exit handler";

        SigsafeRWLock::init();

        lock_guard<mutex> lock(GlobalData::s_mutex);

        if (!GlobalData::sG) {
            GlobalData::sG = new Caliper::GlobalData; 

            // now it is safe to use the Caliper interface

            Caliper c(GlobalData::sG);

            GlobalData::sG->key_attr
                = c.create_attribute("cali.key.attribute", CALI_TYPE_USR, CALI_ATTR_HIDDEN);
            
            Services::register_services(&c);

            Log(1).stream() << "Initialized" << endl;

            if (Log::verbosity() >= 2)
                RuntimeConfig::print( Log(2).stream() << "Configuration:\n" );

            GlobalData::sG->events.post_init_evt(&c);
            
            GlobalData::s_siglock = 0;
        }
    }

    return Caliper(GlobalData::sG);
}

void
Caliper::release()
{
    delete GlobalData::sG;
    GlobalData::sG = 0;
}

// Caliper Caliper::try_instance()
// {
//     return CaliperImpl::s_siglock == 0 ? Caliper(GlobalData::sG) : Caliper(0);
// }
