/*
 *                         Vortex OpenSplice
 *
 *   This software and documentation are Copyright 2006 to TO_YEAR ADLINK
 *   Technology Limited, its affiliated companies and licensors. All rights
 *   reserved.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 *
 */
#include "v__observable.h"
#include "v__observer.h"
#include "v__collection.h"
#include "v__lease.h"
#include "v__entity.h"
#include "v__entry.h"
#include "v__reader.h"
#include "v__index.h"
#include "v__dataReaderEntry.h"
#include "v__dataReaderInstance.h"
#include "v__dataReaderSample.h"
#include "v__dataReader.h"
#include "v__subscriber.h"
#include "v__status.h"
#include "v__query.h"
#include "v__builtin.h"
#include "v__deadLineInstanceList.h"
#include "v__deadLineInstance.h"
#include "v__leaseManager.h"
#include "v__dataView.h"
#include "v__statCat.h"
#include "v__topic.h"
#include "v__kernel.h"
#include "v__policy.h"
#include "v__transaction.h"
#include "v__orderedInstance.h"
#include "v__participant.h"
#include "v_dataReaderStatistics.h"
#include "v_dataReaderEntry.h"
#include "v_state.h"
#include "v_event.h"
#include "v_projection.h"
#include "v_handle.h"
#include "v_group.h"
#include "v_groupSet.h"
#include "v_groupCache.h"
#include "v_topic.h"
#include "v_partition.h"
#include "v_qos.h"
#include "v_public.h"
#include "c_stringSupport.h"
#include "v_kernelParser.h"
#include "v_spliced.h"
#include "v_durabilityClient.h"
#include "v_historicalDataRequest.h"
#include "c_collection.h"

#include "vortex_os.h"
#include "os_report.h"
#include "os_atomics.h"

#include "q_helper.h"

/**************************************************************
 * Instance set macros
 **************************************************************/

#define v_dataReaderAllInstanceSet(_this) \
        (v_dataReader(_this)->index->objects)

#define v_dataReaderNotEmptyInstanceSet(_this) \
        (v_dataReader(_this)->index->notEmptyList)

const char*
v_dataReaderResultString(
    v_dataReaderResult result)
{
    const char *image;

#define V__CASE__(result) case result: image = #result; break;
    switch (result) {
        V__CASE__(V_DATAREADER_INSERTED);
        V__CASE__(V_DATAREADER_OUTDATED);
        V__CASE__(V_DATAREADER_NOT_OWNER);
        V__CASE__(V_DATAREADER_MAX_SAMPLES);
        V__CASE__(V_DATAREADER_MAX_INSTANCES);
        V__CASE__(V_DATAREADER_INSTANCE_FULL);
        V__CASE__(V_DATAREADER_SAMPLE_LOST);
        V__CASE__(V_DATAREADER_DUPLICATE_SAMPLE);
        V__CASE__(V_DATAREADER_OUT_OF_MEMORY);
        V__CASE__(V_DATAREADER_INTERNAL_ERROR);
        V__CASE__(V_DATAREADER_UNDETERMINED);
        V__CASE__(V_DATAREADER_FILTERED_OUT);
        V__CASE__(V_DATAREADER_COUNT);
        default:
            image = "Internal error: no image for illegal result value";
            break;
    }
#undef V__CASE__

    return image;
}

static v_dataReaderInstance
dataReaderLookupInstanceUnlocked(
    v_dataReader _this,
    v_message keyTemplate)
{
    v_dataReaderInstance instance, found;

    assert(C_TYPECHECK(_this,v_dataReader));
    assert(C_TYPECHECK(keyTemplate,v_message));

    /* The following line must be removed a.s.a.p.
     * Its not intended to modify the template but there exist a dependency of
     * this side effect that needs to be removed first.
     */
    v_nodeState(keyTemplate) = L_REGISTER;

    instance = v_dataReaderInstanceNew(_this, keyTemplate);

    if (instance == NULL) {
        OS_REPORT(OS_ERROR,
                    "kernel::v_dataReader::dataReaderLookupInstanceUnlocked", V_RESULT_INTERNAL_ERROR,
                    "Operation v_dataReaderInstanceNew(_this=0x%"PA_PRIxADDR", keyTemplate=0x%"PA_PRIxADDR") failed.",
                     (os_address)_this, (os_address)keyTemplate);
        found = NULL;
    } else {
        if (v_dataReaderQos(_this)->userKey.v.enable) {
            /* In case of user defined keys the NotEmpty instance set contains all
             * instances by definition and therefore the objects set that normally
             * contains all instances is not used.
             * So in that case the lookup instance must act on the Not Empty
             * instance set.
             */
            found = c_find(v_dataReaderNotEmptyInstanceSet(_this),instance);
        } else {
            found = c_find(v_dataReaderAllInstanceSet(_this), instance);
        }

        if (found != NULL) {
            if (v_dataReaderInstanceEmpty(found)) {
                if (v_stateTest(v_instanceState(found), L_NOWRITERS)) {
                    c_free(found);
                    found = NULL;
                }
            }
        }
        v_dataReaderInstanceFree(instance);
    }

    return found;
}

struct onNewIndexArg {
    v_dataReader dataReader;
    q_expr _where;
    const c_value **params;
    os_uint32 nrOfParams;
};

static c_bool
onNewIndex(
    v_index index,
    v_topic topic,
    c_voidp arg)
{
    struct onNewIndexArg *a = (struct onNewIndexArg *)arg;
    v_dataReaderEntry entry;

    entry = v_dataReaderEntryNew(a->dataReader, topic, a->_where, a->params, a->nrOfParams);
    if (entry) {
        v_dataReaderAddEntry(a->dataReader,entry);
        entry->index = c_keep(index);
        index->entry = entry;
        c_free(entry);
        return TRUE;
    } else {
        return FALSE;
    }
}

/* Precondition: dataReader is locked (OSPL_LOCK). */

c_ulong
v_dataReaderInstanceCount_nl(
    v_dataReader _this)
{
    assert(C_TYPECHECK(_this,v_dataReader));
    OSPL_ASSERT_LOCK(_this);

    return c_tableCount(v_dataReaderAllInstanceSet(_this));
}

void
v_dataReaderUpdatePurgeLists(
    v_dataReader _this)
{
    assert(C_TYPECHECK(_this,v_dataReader));
    v_dataReaderEntryUpdatePurgeLists(_this->entry);
}

void
v_dataReaderBeginAccess(
    v_dataReader _this)
{
    assert(C_TYPECHECK(_this,v_dataReader));

    OSPL_LOCK(_this);
    _this->accessBusy = TRUE;
    v_dataReaderEntryUpdatePurgeLists(_this->entry);
    OSPL_UNLOCK(_this);
}

static c_bool
flushPending(
    c_object o,
    c_voidp arg)
{
    v_dataReaderInstance instance = v_dataReaderInstance(o);
    c_bool inNotEmptyList;

    OS_UNUSED_ARG(arg);

    inNotEmptyList = v_dataReaderInstanceInNotEmptyList(instance);

    v_dataReaderInstanceFlushPending(instance);

    if (!v_dataReaderInstanceEmpty(instance) && !inNotEmptyList) {
        c_tableInsert(v_dataReaderInstanceReader(instance)->index->notEmptyList, instance);
        v_dataReaderInstanceInNotEmptyList(instance) = TRUE;
    }

    return TRUE;
}

void
v_dataReaderEndAccess(
    v_dataReader _this)
{
    assert(_this != NULL);
    assert(C_TYPECHECK(_this,v_dataReader));

    OSPL_LOCK(_this);
    (void) c_tableWalk(v_dataReaderAllInstanceSet(_this), flushPending, NULL);
    v_dataReaderUpdatePurgeLists(_this);
    _this->accessBusy = FALSE;

    OSPL_UNLOCK(_this);
}

#ifdef _MSG_STAMP_

#define _LAPSTAMP_(_this,_lap) { \
            if ((_lap > 0) && (_lap < 100000000))  { \
            _this.min = (_this.min < _lap ? _this.min : _lap); \
            _this.max = (_this.max > _lap ? _this.max : _lap); \
            _this.tot += _lap; \
            _this.count++; \
            } \
        }

void
v_dataReaderLogMessage(
    v_dataReader _this,
    v_message msg)
{
    v_hrtime lap;
    c_long i;

    for (i=0; i<=msg->hops; i++) {
        lap = msg->writerCopyTime[i] - msg->writerAllocTime[i];
        _LAPSTAMP_(_this->writerCopyTime[i],lap);
        lap = msg->writerLookupTime[i] - msg->writerCopyTime[i];
        _LAPSTAMP_(_this->writerLookupTime[i],lap);
        lap = msg->groupInsertTime[i] - msg->writerLookupTime[i];
        _LAPSTAMP_(_this->writerWriteTime[i],lap);
        lap = msg->groupLookupTime[i] - msg->groupInsertTime[i];
        _LAPSTAMP_(_this->groupLookupTime[i],lap);
        lap = msg->readerInsertTime[i] - msg->groupLookupTime[i];
        _LAPSTAMP_(_this->groupWriteTime[i],lap);
        lap = msg->readerLookupTime[i] - msg->readerInsertTime[i];
        _LAPSTAMP_(_this->readerLookupTime[i],lap);
        lap = msg->readerDataAvailableTime[i] - msg->readerLookupTime[i];
        _LAPSTAMP_(_this->readerInstanceTime[i],lap);
        lap = msg->readerInstanceTime[i] - msg->readerDataAvailableTime[i];
        _LAPSTAMP_(_this->readerInsertTime[i],lap);
        lap = msg->readerNotifyTime[i] - msg->readerInstanceTime[i];
        _LAPSTAMP_(_this->readerNotifyTime[i],lap);
        lap = msg->readerReadTime[i] - msg->readerNotifyTime[i];
        _LAPSTAMP_(_this->readerReadTime[i],lap);
        lap = msg->readerCopyTime[i] - msg->readerReadTime[i];
        _LAPSTAMP_(_this->readerCopyTime[i],lap);

        lap = msg->nwSerialisationTime[i] - msg->readerDataAvailableTime[i];
        _LAPSTAMP_(_this->nwSerialisationTime[i],lap);
        lap = msg->nwBufferFullTime[i] - msg->readerDataAvailableTime[i];
        _LAPSTAMP_(_this->nwBufferFullTime[i],lap);
        lap = msg->nwFlushBufferTime[i] - msg->nwBufferFullTime[i];
        _LAPSTAMP_(_this->nwFlushBufferTime[i],lap);
        lap = msg->nwSendTime[i] - msg->nwFlushBufferTime[i];
        _LAPSTAMP_(_this->nwSendTime[i],lap);
        if (i > 0) {
            lap = msg->nwReceiveTime[i] - msg->nwSendTime[i-1];
            _LAPSTAMP_(_this->nwReceiveTime[i],lap);
        }
        lap = msg->nwInsertTime[i] - msg->nwReceiveTime[i];
        _LAPSTAMP_(_this->nwInsertTime[i],lap);
    }
}
#undef _LAPSTAMP_

#define _LAPINIT_(_this) { \
            _this.cur = 0ll; \
            _this.min = 0x7fffffffffffffffll; \
            _this.max = 0ll; \
            _this.tot = 0ll; \
            _this.count = 0; \
        }

static void
v_dataReaderLogInit(
    v_dataReader _this)
{
    c_long i;

    for (i=0; i<2; i++) {
        _LAPINIT_(_this->writerCopyTime[i]);
        _LAPINIT_(_this->writerLookupTime[i]);
        _LAPINIT_(_this->writerWriteTime[i]);
        _LAPINIT_(_this->groupLookupTime[i]);
        _LAPINIT_(_this->groupWriteTime[i]);
        _LAPINIT_(_this->readerLookupTime[i]);
        _LAPINIT_(_this->readerInstanceTime[i]);
        _LAPINIT_(_this->readerInsertTime[i]);
        _LAPINIT_(_this->readerNotifyTime[i]);
        _LAPINIT_(_this->readerReadTime[i]);
        _LAPINIT_(_this->readerCopyTime[i]);
        _LAPINIT_(_this->nwSerialisationTime[i]);
        _LAPINIT_(_this->nwBufferFullTime[i]);
        _LAPINIT_(_this->nwFlushBufferTime[i]);
        _LAPINIT_(_this->nwSendTime[i]);
        _LAPINIT_(_this->nwReceiveTime[i]);
        _LAPINIT_(_this->nwInsertTime[i]);
    }
}
#undef _LAPINIT_

static void
_lapReport(
    v_laptime *laptime,
    const char *info)
{
    if (laptime->count > 0) {
        printf("%10d %16d %16d %16d (%s)\n",
            laptime->count,
            (int)(laptime->tot / 1000ll) / laptime->count,
            (int)(laptime->min / 1000ll),
            (int)(laptime->max / 1000ll),
            info);
    }
}

static void
v_dataReaderLogReport(
    v_dataReader _this)
{
    c_char *info = "DataReader message arrival stats";
    c_long i;

    if (_this->writerLookupTime[0].count > 99) {
        printf("==============================================================\n");
        printf("DataReader : %s\n", info);
        printf("--------------------------------------------------------------\n");
        printf("Nr of laps       mean (usec)       min (usec)       max (usec)\n");
        for (i=0; i<2; i++) {
            printf("--------------------------------------------------------------\n");
            _lapReport(&_this->nwReceiveTime[i],"Network: receive time");
            _lapReport(&_this->nwInsertTime[i],"Network: insert time");
            _lapReport(&_this->writerCopyTime[i],"Writer: copy data time");
            _lapReport(&_this->writerLookupTime[i],"Writer: lookup instance time");
            _lapReport(&_this->writerWriteTime[i],"Writer: write to group time");
            _lapReport(&_this->groupLookupTime[i],"Group: lookup instance time");
            _lapReport(&_this->groupWriteTime[i],"Group: write to Reader time");
            _lapReport(&_this->readerLookupTime[i],"Reader: lookup instance time");
            _lapReport(&_this->readerInstanceTime[i],"Reader: insert instance time");
            _lapReport(&_this->readerInsertTime[i],"Reader: insert time");
            _lapReport(&_this->readerNotifyTime[i],"Reader: Notify time");
            _lapReport(&_this->readerReadTime[i],"Reader: read time");
            _lapReport(&_this->readerCopyTime[i],"Reader: copy time");
            _lapReport(&_this->nwSerialisationTime[i],"Network: serialisation time");
            _lapReport(&_this->nwBufferFullTime[i],"Network: buffer full time");
            _lapReport(&_this->nwFlushBufferTime[i],"Network: buffer flush time");
            _lapReport(&_this->nwSendTime[i],"Network: send time");

        }
        printf("==============================================================\n");
    }
}

#endif

v_dataReader
v_dataReaderNewBySQL (
    v_subscriber subscriber,
    const os_char *name,
    const os_char *expr,
    const c_value params[],
    os_uint32 nrOfParams,
    v_readerQos qos)
{
    v_dataReader reader = NULL;
    q_expr OQLexpr;

    if (expr) {
        OQLexpr = v_parser_parse(expr);
        reader = v_dataReaderNew(subscriber, name, OQLexpr, params, nrOfParams, qos);
        q_dispose(OQLexpr);
    }
    return reader;
}

v_dataReader
v_dataReaderNew (
    _In_ v_subscriber subscriber,
    _In_opt_z_ const c_char *name,
    _In_opt_ q_expr OQLexpr,
    const c_value params[],
    os_uint32 nrOfParams,
    _In_opt_ v_readerQos qos)
{
    v_kernel kernel;
    v_participant participant;
    v_dataReader _this, found;
    v_readerQos q;
    struct onNewIndexArg arg;
    q_expr expr, term, _projection, _from, _where;
    c_type instanceType;
    c_property sampleProperty;
    c_long i;
    v_topic topic;
    v_result result;

    if (name == NULL) {
        name = "<No Name>";
    }
    assert(C_TYPECHECK(subscriber,v_subscriber));
    kernel = v_objectKernel(subscriber);

    if (!q_isFnc(OQLexpr,Q_EXPR_PROGRAM)) {
        OS_REPORT(OS_ERROR,
                    "kernel::v_dataReader::v_dataReaderNew",V_RESULT_INTERNAL_ERROR,
                    "Operation for Datareader (name=\"%s\") failed:"
                    OS_REPORT_NL "Reason: expression=0x%"PA_PRIxADDR" is not a valid view expression.",
                    name, (os_address)OQLexpr);
        return NULL;
    }
    expr = q_getPar(OQLexpr,0);
    if (!q_isFnc(expr,Q_EXPR_SELECT)) {
        OS_REPORT(OS_ERROR,
                    "kernel::v_dataReader::v_dataReaderNew",V_RESULT_INTERNAL_ERROR,
                    "Operation for Datareader (name=\"%s\") failed:"
                    OS_REPORT_NL "Reason: expression=0x%"PA_PRIxADDR" is not a valid select statement.",
                    name, (os_address)OQLexpr);
        return NULL;
    }

    _projection = NULL;
    _from = NULL;
    _where = NULL;
    i=0;
    term = q_getPar(expr,i++);
    while (term != NULL) {
        switch (q_getTag(term)) {
        case Q_EXPR_PROJECTION:
            _projection = term;
        break;
        case Q_EXPR_FROM:
            _from = q_getPar(term,0);
        break;
        case Q_EXPR_WHERE:
            _where = term;
        break;
        default:
        break;
        }
        term = q_getPar(expr,i++);
    }
    if (_from == NULL) {
        OS_REPORT(OS_ERROR,
                    "kernel::v_dataReader::v_dataReaderNew",V_RESULT_ILL_PARAM,
                    "Operation for Datareader (name=\"%s\") failed:"
                    OS_REPORT_NL "Reason: Missing from clause in expression 0x%"PA_PRIxADDR".",
                    name, (os_address)OQLexpr);
        return NULL;
    }

    /* ES, dds1576: Before creating the datareader we have to verify that read
     * access to the topic is allowed. We can accomplish this by checking the
     * access mode of the topic.
     */
    topic = v_lookupTopic (kernel, q_getId(_from));
    if(!topic)
    {
        OS_REPORT(OS_ERROR,
                    "kernel::v_dataReader::v_dataReaderNew",V_RESULT_ILL_PARAM,
                    "DataReader (name=\"%s\") not created: "
                    "Could not locate topic with name \"%s\".",
                    name, q_getId(_from));
        return NULL;
    }
    if(v_topicAccessMode(topic) != V_ACCESS_MODE_READ &&
       v_topicAccessMode(topic) != V_ACCESS_MODE_READ_WRITE)
    {
        OS_REPORT(OS_ERROR,
                    "kernel::v_dataReader::v_dataReaderNew", V_RESULT_ILL_PARAM,
                    "Creation of DataReader (name=\"%s\") failed."
                    OS_REPORT_NL "Topic (name=\"%s\") does not have read access rights.",
                    name, q_getId(_from));
        c_free(topic);
        return NULL;
    }
    c_free(topic);
    topic = NULL;

    if (v_readerQosCheck(qos) == V_RESULT_OK) {
        q = v_readerQosNew(kernel, qos);
        if (q == NULL) {
            OS_REPORT(OS_ERROR,
                        "kernel::v_dataReader::v_dataReaderNew", V_RESULT_INTERNAL_ERROR,
                        "Creation of DataReader (name=\"%s\") failed. Cannot create reader QoS.",
                        name);
            return NULL;
        }
    } else {
        return NULL;
    }

    if (q->share.v.enable && !subscriber->qos->share.v.enable){
        OS_REPORT(OS_ERROR, "kernel::v_dataReader::v_dataReaderNew", V_RESULT_PRECONDITION_NOT_MET,
                    "Datareader (name=\"%s\") not created."
                    OS_REPORT_NL "Can't create a shared reader in a non-shared subscriber.",
                    name);
        c_free(q);
        return NULL;
    }

    _this = v_dataReader(v_objectNew(kernel, K_DATAREADER));

    if (v_isEnabledStatistics(kernel, V_STATCAT_READER)) {
        _this->statistics = v_dataReaderStatisticsNew(kernel);
    }

    v_readerInit(v_reader(_this), name, subscriber, q);

    /* Not really required to lock during creation as no other thread has acces yet.
     * However to avoid warnings from static code analisys this lock is performed anyway.
     */
    OSPL_LOCK(_this);

    _this->shareCount = 1;
    _this->views = NULL;
    _this->resourceSampleCount = 0;
    _this->notReadCount = 0;
    _this->maxInstances = FALSE;
    _this->triggerValue = NULL;
    _this->walkRequired = TRUE;
    _this->readCnt = 0;
    _this->accessBusy = FALSE;
    /* temporary until set property is implemented */
    _this->maximumSeparationTime = 0;
    if (qos) {
        _this->maximumSeparationTime = qos->pacing.v.minSeperation;
    }

    OSPL_UNLOCK(_this);

#ifdef _MSG_STAMP_
    v_dataReaderLogInit(_this);
#endif

    if (q->share.v.enable) {
        assert(subscriber->qos->share.v.enable);
        found = v_subscriberAddShare(subscriber,_this);
        if (found != _this) {
           /* Existing shared DataReader so abort reader creation and
            * return existing DataReader.
            *
            * Make sure to set the index and deadline list to NULL,
            * because v_publicFree will cause a crash in the
            * v_dataReaderDeinit otherwise.
            */
            _this->entry = NULL;
            _this->index = NULL;
            _this->deadLineList = NULL;
            _this->minimumSeparationList = NULL;
            _this->minimumSeparationLease = NULL;
            /*v_publicFree to free reference held by the handle server.*/
            v_publicFree(v_public(_this));
            /*Now free the local reference as well.*/
            c_free(_this);
            c_free(q);
            return c_keep(found);
        }
    }
    c_free(q);

    arg.dataReader = _this;
    arg._where = _where;
    arg.params = &params;
    arg.nrOfParams = nrOfParams;
    _this->index = v_indexNew(_this, _from, onNewIndex, &arg);
    if (_this->index != NULL) {
        _this->entry = c_keep(_this->index->entry);
        instanceType = v_dataReaderInstanceType(_this);
        sampleProperty = c_property(c_metaResolve(c_metaObject(instanceType), "sample"));
        c_free(instanceType);

        if (sampleProperty != NULL) {
            result = V_RESULT_OK;
        } else {
            result = V_RESULT_INTERNAL_ERROR;
            OS_REPORT(OS_ERROR,
                "kernel::v_dataReader::v_dataReaderNew", result,
                "Creation of DataReader (name=\"%s\") failed: "
                OS_REPORT_NL "Operation c_metaResolve(scope=0x%"PA_PRIxADDR", \"sample\") failed.",
                name, (os_address)instanceType);
        }
    } else {
        result = V_RESULT_INTERNAL_ERROR;
        OS_REPORT(OS_ERROR,
            "kernel::v_dataReader::v_dataReaderNew", result,
            "Creation of DataReader (name=\"%s\") failed: v_indexNew failed.",
            name);
    }

    if (result == V_RESULT_OK) {
        _this->sampleType = c_keep (sampleProperty->type);
        _this->projection = v_projectionNew(_this,_projection);
        _this->orderedInstance = NULL;

        participant = v_participant(subscriber->participant);
        assert(participant != NULL);
        _this->deadLineList = v_deadLineInstanceListNew(c_getBase(c_object(_this)),
                                  participant->leaseManager,
                                  q->deadline.v.period,
                                  V_LEASEACTION_READER_DEADLINE_MISSED,
                                  v_public(_this));

        result = v_subscriberAddReader (subscriber, v_reader (_this));
    }

    if (result != V_RESULT_OK) {
        v_readerDeinit(v_reader(_this));
        c_free(_this);
        _this = NULL;
    }
    return _this;
}

struct mp_arg_s {
    const struct v_subscriptionInfo *info;
    v_dataReaderEntry entry;
    v_participant participant;
};

static os_boolean
getMatchingPublications(
    const v_message publication,
    void *arg)
{
    struct mp_arg_s *a = (struct mp_arg_s *)arg;
    const struct v_publicationInfo *info = (struct v_publicationInfo *)(publication + 1);

    if ((strcmp(info->topic_name, a->info->topic_name) == 0) &&
        v_builtinTestPartitionMatch(info, a->info) &&
        v_builtinTestQosMatch(info, a->info))
    {
        TRACE_IGNORE("DataReader::Enable: found matching publication GID: {%d, %d, %d}\n",
                     info->key.systemId, info->key.localId, info->key.serial);
        /* Found a matching publication so now check with the particinant if it ought to be ignored! */
        if (v_participantCheckPublicationIgnored(a->participant, info) == OS_TRUE) {
            v_dataReaderEntryIgnorePublication(a->entry, info);
        }
    }
    return OS_TRUE;
}

/* This operation is called during reader enable and initializes the readers ignore list by
 * first lookup all matching (discovered) publications and the check if any are ignored according
 * to the participant's ignore list.
 */
static void
initializeIgnoreList(
    v_dataReader _this)
{
    v_kernel kernel;
    v_message subscription;
    struct mp_arg_s arg;

    kernel = v_objectKernel(_this);
    subscription = v_dataReaderSubscription(_this);
    if (subscription != NULL) {
        arg.participant = v_subscriberParticipant(v_readerSubscriber(_this));
        if (arg.participant->ignore) {
            arg.entry = _this->entry;
            arg.info = (const struct v_subscriptionInfo *) (subscription + 1);
            v_kernelWalkPublications(kernel, getMatchingPublications, &arg);
        }
        c_free(subscription);
    }
}

v_result
v_dataReaderEnable(
    _Inout_ v_dataReader _this)
{
    v_kernel kernel;
    v_message builtinCMMsg;
    v_subscriber subscriber;
    v_result result = V_RESULT_OK;

    subscriber = v_subscriber(v_reader(_this)->subscriber);
    kernel = v_objectKernel(_this);

    /* A datareader may be enabled if the subscriber is either enabling or enabled */
    if(v_entityDisabled(v_entity(subscriber))) {
        /* DDS v1.4 $2.1.2.1.1.7: Calling enable on an Entity whose factory is
         * not enabled will fail and return PRECONDITION_NOT_MET.
         */
        return V_RESULT_PRECONDITION_NOT_MET;
    }

    c_free(_this->subInfo);
    _this->subInfo = v_builtinCreateSubscriptionInfo(kernel->builtin, v_reader(_this));
    builtinCMMsg = v_builtinCreateCMDataReaderInfo(kernel->builtin, v_reader(_this));

    initializeIgnoreList(_this);

    result = v_subscriberEnableReader (subscriber, v_reader (_this));
    if (result == V_RESULT_OK) {
        v_writeBuiltinTopic(kernel, V_SUBSCRIPTIONINFO_ID, _this->subInfo);
        v_writeBuiltinTopic(kernel, V_CMDATAREADERINFO_ID, builtinCMMsg);

        /* Trigger the durability client in spliced to send a historical data request
         * for non-volatile readers in case the reader is not yet complete
         */
        if ((v_reader(_this)->qos->durability.v.kind != V_DURABILITY_VOLATILE)) {
            (void) v_readerWaitForHistoricalData(v_reader(_this), OS_DURATION_ZERO, TRUE);
        }
    }
    c_free(builtinCMMsg);
    return result;
}

static c_bool
instanceFree(
    c_object o,
    c_voidp arg)
{
    OS_UNUSED_ARG(arg);
    v_dataReaderInstanceFree(v_dataReaderInstance(o));
    return TRUE;
}

void
v_dataReaderFree (
    v_dataReader _this)
{
    v_message builtinMsg, builtinCMMsg;
    v_message unregisterMsg, unregisterCMMsg;
    v_subscriber subscriber;
    v_kernel kernel;
    v_dataView view;
    c_iter views;
    c_bool userKey;
    c_bool enabled = FALSE;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this,v_dataReader));

#ifdef _MSG_STAMP_
    v_dataReaderLogReport(_this);
#endif
    /* First create message, only at the end dispose. Applications expect
     * the disposed sample to be the last!
     */
    subscriber = v_subscriber(v_reader(_this)->subscriber);
    assert(subscriber);
    if (v_reader(_this)->qos->share.v.enable) {
        if (v_subscriberRemoveShare(subscriber,_this) > 0) {
            /* Not the last reference, so not destroy yet */
            return;
        }
    }

    kernel = v_objectKernel(_this);
    builtinMsg = v_builtinCreateSubscriptionInfo(kernel->builtin,v_reader(_this));
    builtinCMMsg = v_builtinCreateCMDataReaderInfo(kernel->builtin,v_reader(_this));
    unregisterMsg = v_builtinCreateSubscriptionInfo(kernel->builtin,v_reader(_this));
    unregisterCMMsg = v_builtinCreateCMDataReaderInfo(kernel->builtin,v_reader(_this));

    userKey = v_reader(_this)->qos->userKey.v.enable;
    v_readerFree(v_reader(_this));
    OSPL_LOCK(_this);

    enabled = v__entityEnabled_nl(v_entity(_this));
    v_orderedInstanceRemove(_this->orderedInstance, v_entity(_this));

    if(_this->deadLineList){
        v_deadLineInstanceListFree(_this->deadLineList);
    }
    if (_this->minimumSeparationLease) {
        v_leaseManagerDeregister(v_participant(subscriber->participant)->leaseManager,
                                 _this->minimumSeparationLease);
        c_free(_this->minimumSeparationLease);
        _this->minimumSeparationLease = NULL;
    }
    if (_this->views != NULL) {
        views = ospl_c_select(_this->views, 0);
        view = v_dataView(c_iterTakeFirst(views));
        while (view != NULL) {
            v_dataViewFreeUnsafe(view);
            view = v_dataView(c_iterTakeFirst(views));
        }
        c_iterFree(views);
    }
    if (_this->triggerValue) {
        v_dataReaderTriggerValueFree(_this->triggerValue);
        _this->triggerValue = NULL;
    }
    if(_this->index){
        if (userKey) {
            (void) c_tableWalk(v_dataReaderNotEmptyInstanceSet(_this), instanceFree, NULL); /* Always returns TRUE. */
        } else {
            (void) c_tableWalk(v_dataReaderAllInstanceSet(_this), instanceFree, NULL); /* Always returns TRUE. */
        }
    }
    OSPL_UNLOCK(_this);

    if (enabled) {
        v_writeDisposeBuiltinTopic(kernel, V_SUBSCRIPTIONINFO_ID, builtinMsg);
        v_writeDisposeBuiltinTopic(kernel, V_CMDATAREADERINFO_ID, builtinCMMsg);
        v_unregisterBuiltinTopic(kernel, V_SUBSCRIPTIONINFO_ID, unregisterMsg);
        v_unregisterBuiltinTopic(kernel, V_CMDATAREADERINFO_ID, unregisterCMMsg);
    }
    c_free(builtinMsg);
    c_free(builtinCMMsg);
    c_free(unregisterMsg);
    c_free(unregisterCMMsg);
}

static c_bool
resetInstanceOwner(
    c_object obj,
    c_voidp arg)
{
    v_dataReaderInstanceResetOwner(v_dataReaderInstance(obj), *((v_gid*)(arg)));

    return TRUE;
}

void
v_dataReaderDeinit (
    v_dataReader _this)
{
    assert(_this != NULL);
    assert(C_TYPECHECK(_this,v_dataReader));
    v_readerDeinit(v_reader(_this));
}

/* Help function for writing into the dataViews */

static v_actionResult
writeSlave(
    c_object sample,
    c_voidp arg)
{
    v_actionResult result = V_PROCEED;
    if (v_readerSampleTestState(sample, L_VALIDDATA))
    {
        result = v_dataViewWrite(v_dataView(arg), v_readerSample(sample));
    }
    return result;
}

static c_bool
walkInstanceSamples(
    c_object o,
    c_voidp arg)
{
    (void)v_dataReaderInstanceWalkSamples(v_dataReaderInstance(o), writeSlave, arg);
    return TRUE;
}

void
v_dataReaderInsertView(
    v_dataReader _this,
    v_dataView view)
{
    c_base base;
    c_type type;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this, v_dataReader));
    assert(view != NULL);
    assert(C_TYPECHECK(view, v_dataView));

    OSPL_LOCK(_this);
    /* Create set if it does not exist yet */
    if (_this->views == NULL) {
        base = c_getBase((c_object)_this);
        type = c_resolve(base, "kernelModuleI::v_dataView");
        _this->views = c_setNew(type);
    }
    /* Insert the view in the set */
    ospl_c_insert(_this->views, view);
    /* Fill the view with initial data */
    c_tableWalk(v_dataReaderNotEmptyInstanceSet(_this),
                walkInstanceSamples,
                view);

    OSPL_UNLOCK(_this);
}

void
v_dataReaderRemoveViewUnsafe(
    v_dataReader _this,
    v_dataView view)
{
    v_dataView found;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this, v_dataReader));
    assert(view != NULL);
    assert(C_TYPECHECK(view, v_dataView));

    if (_this->views != NULL) {
        found = c_remove(_this->views, view, NULL, NULL);
        assert(found == view);
        if (found == view) {
            c_free(found);
            if (c_count(_this->views) == 0) {
                c_free(_this->views);
                _this->views = NULL;
            }
        }
    }
    /* Remove all data from the view */
    v_dataViewWipeSamples(view);
}

void
v_dataReaderRemoveView(
    v_dataReader _this,
    v_dataView view)
{
    assert(_this != NULL);
    assert(C_TYPECHECK(_this, v_dataReader));
    assert(view != NULL);
    assert(C_TYPECHECK(view, v_dataView));

    OSPL_LOCK(_this);
    v_dataReaderRemoveViewUnsafe(_this, view);
    OSPL_UNLOCK(_this);
}

C_STRUCT(readSampleArg) {
    v_dataReader reader;
    v_sampleMask mask;
    c_query query;
    v_readerSampleAction action;
    c_voidp arg;
    c_long count;
    c_iter emptyList;
};
C_CLASS(readSampleArg);

c_bool
v__dataReaderWalkInstances(
    v_dataReader _this,
    v_dataReaderInstanceAction action,
    c_voidp arg)
{
    assert (_this != NULL && C_TYPECHECK (_this, v_dataReader));

    return c_readAction (
        v_dataReaderNotEmptyInstanceSet (_this), (c_action)action, arg);
}

c_bool
v_dataReaderWalkInstances(
    v_dataReader _this,
    v_dataReaderInstanceAction action,
    c_voidp arg)
{
    c_bool proceed;

    if (_this != NULL) {
        OSPL_LOCK(_this);
        proceed = v__dataReaderWalkInstances (_this, action, arg);
        OSPL_UNLOCK(_this);
    } else {
        proceed = FALSE;
        OS_REPORT(OS_ERROR,"v_dataReaderWalkInstances",V_RESULT_ILL_PARAM,
                   "dataReader object is NULL");
    }
    assert(_this != NULL);

    return proceed;

}

static v_actionResult
instanceSampleAction(
    c_object sample,
    c_voidp arg)
{
    readSampleArg a = (readSampleArg)arg;
    a->count++;
    return a->action(sample,a->arg);
}

static c_bool
instanceReadSamples(
    v_dataReaderInstance instance,
    c_voidp arg)
{
    readSampleArg a = (readSampleArg)arg;
    c_bool proceed = TRUE;

    assert(C_TYPECHECK(a->reader, v_dataReader));

    if (!v_dataReaderInstanceEmpty(instance)) {
        proceed = v_dataReaderInstanceReadSamples(instance, a->query, a->mask, instanceSampleAction, arg);
        assert(v_dataReader(a->reader)->resourceSampleCount >= 0);
        /* Note that if the instance has become empty it is not
         * removed from the not empty list yet!
         * An empty instance will be removed the next time the instance
         * is accessed (see the following else branch).
         * This is an optimization to avoid continuous inserting and
         * removing instances in case samples are continuous written
         * and taken.
         */
    } else {
        if (v_dataReaderInstanceInNotEmptyList(instance)) {
            /* Apparently the instance was already empty and as
             * optimization described above left in the not empty list.
             * And no data is inserted in the meanwhile meaning that
             * the use case for optimization is not the case.
             * So the instance can now be registered to be removed.
             */
            c_keep(instance);
            a->emptyList = c_iterInsert(a->emptyList,instance);
        }
    }

    return proceed;
}

static void
resetCommunicationStatusFlags(
    v_dataReader _this)
{
    v_entity parent;

    v_statusReset(v_entity(_this)->status,V_EVENT_DATA_AVAILABLE);
    parent = v_entityOwner(v_entity(_this));
    while (parent) {
        if (parent->status) {
            v_statusReset(parent->status,V_EVENT_ON_DATA_ON_READERS);
        }
        parent = v_entityOwner(v_entity(parent));
    }
}

static v_result
v__dataReaderOrderedReadOrTake (
    v_dataReader _this,
    v_sampleMask mask,
    v__dataReaderAction readOrTake,
    v_readerSampleAction action,
    c_voidp argument)
{
    v_result result = V_RESULT_NO_DATA;
    v_actionResult proceed = V_PROCEED;
    v_dataReader reader;
    v_dataReaderInstance instance;
    v_dataReaderSample bookmark, first, sample;

    assert (_this != NULL && C_TYPECHECK (_this, v_dataReader));
    assert (action != NULL);
    assert (argument != NULL);

    /* The following use-cases describe the behavior of the read operations and how
     * this function is implemented with respect to this behavior.
     * This description is applicable to topic and instance scope access
     * (group scope access uses a read list that does not support cyclic reads).
     *
     * Local variables used in this function:
     *  - first = the head of the v_orderedInstance. (if NULL there is no data to be read)
     *  - bookmark = the first sample read (starting point in the v_orderedInstance).
     *  - sample = current read position in the v_orderedInstance. (if NULL there is no more data to be read)
     *
     * 1) The reader has no samples:
     *    Result   : On read/take nothing is read and NO_DATA is returned.
     *    End state: sample, bookmark and first are NULL.
     *    Action   : The v_orderedInstance will be reset, meaning that the internal bookmark
     *               of the v_orderedInstance is set to the head which in this case is a no-op.
     *    Next read: Newly inserted samples will be read next time because of the reset.
     *
     * 2) The reader has the exact number of samples that fits in the user sequence on read/take:
     *    Result   : On read/take all samples are returned.
     *    End state: sample is !NULL, bookmark is !NULL and !first.
     *               v_orderedInstance bookmark is head (next read sample is NULL).
     *    Action   : sample != NULL so v_orderedInstance will not be reset,
     *    Next read: next read sample is NULL and will therefore result in NO_DATA being returned.
     *               This indicates that all samples in the orderedInstance are read.
     *
     * 3) The user sequence has more space then samples available in the reader:
     *    Result   : On read/take all samples are returned.
     *    End state: sample is NULL, bookmark is first.
     *               v_orderedInstance bookmark is head (all samples read, next read is normal from head).
     *    Action   : v_orderedInstance will be reset (no-op).
     *    Next read: will return samples if available (according to all applicable conditions).
     *
     * 4) sequence is long enough this time after n number of reads:
     *    End state: sample is NULL, bookmark is !NULL and !first.
     *               v_orderedInstance bookmark is head (all samples read, next read is normal from head).
     *    Action   : bookmark != first so v_orderedInstance will not be reset,
     *    Note     : User should compare number of samples returned to maximum.
     *               This indicates that all samples in the orderedInstance are read.
     *    Next read: will return samples if available (according to all applicable conditions).
     */

    first = v_orderedInstanceFirstSample (_this->orderedInstance);
    sample = bookmark = v_orderedInstanceReadSample (_this->orderedInstance, mask);
    while (v_actionResultTest (proceed, V_PROCEED) && sample != NULL) {
        instance = v_dataReaderSampleInstance (sample);
        reader = v_dataReaderInstanceReader (instance);

        if (reader != _this) {
            result = V_RESULT_PRECONDITION_NOT_MET;
            OS_REPORT (OS_ERROR, OS_FUNCTION, result,
                "out of order read while ordered access was requested");
            v_orderedInstanceUnaligned (_this->orderedInstance);
            v_actionResultClear (proceed, V_PROCEED);
        } else {
            if (v_sampleMaskPass (mask, sample)) {
                if (v_readerSampleTestState(sample, L_VALIDDATA) ||
                        (
                            v_reader(reader)->qos->lifecycle.v.enable_invalid_samples &&
                            !hasValidSampleAccessible(instance) &&
                            v_dataReaderInstanceStateTest(instance, L_STATECHANGED) &&
                            !v_readerSampleTestStateOr(sample, L_READ | L_LAZYREAD)
                        )
                   )
                {
                    proceed = readOrTake (sample, action, argument);
                    result = V_RESULT_OK;
                } else {
                    v_dataReaderInstanceSampleRemove(instance, sample, FALSE);
                }

            }

            if (v_actionResultTest (proceed, V_PROCEED)) {
                if (v_readerAccessScope(_this) != V_PRESENTATION_GROUP) {
                    sample = v_orderedInstanceReadSample(_this->orderedInstance, mask);
                } else {
                    v_actionResultClear (proceed, V_PROCEED);
                }
            }
        }
    }

    if (sample == NULL && bookmark == first) {
        v_orderedInstanceReset (_this->orderedInstance);
    }

    return result;
}

static v_result
waitForData(
    v_dataReader _this,
    os_duration *delay)
{
    v_result result = V_RESULT_OK;
    /* If no data read then wait for data or timeout. */
    if (*delay > 0) {
        c_ulong flags = 0;
        os_timeE time = os_timeEGet();
        v_observerSetEvent(v_observer(_this), V_EVENT_DATA_AVAILABLE);
        flags = OSPL_CATCH_EVENT(_this, *delay);
        if (flags & V_EVENT_TIMEOUT) {
            result = V_RESULT_TIMEOUT;
        } else {
            *delay -= os_timeEDiff(os_timeEGet(), time);
        }
    } else {
        result = V_RESULT_NO_DATA;
    }
    return result;
}

/* The read and take actions */
v_result
v_dataReaderRead(
    v_dataReader _this,
    v_sampleMask mask,
    v_readerSampleAction action,
    c_voidp arg,
    os_duration timeout)
{
    v_result result = V_RESULT_OK;
    v_dataReaderInstance emptyInstance;
    c_bool unordered = TRUE;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this, v_dataReader));

    /* Collect entries for purging outside of observer lock to prevent deadlock
     * between observer, entrySet and group locks with three different threads.
     */

    OSPL_LOCK(_this);
    if(!v__entityEnabled_nl(v_entity(_this))) {
        OSPL_UNLOCK(_this);
        return V_RESULT_NOT_ENABLED;
    }
    if (v_readerSubscriber(_this) == NULL) {
        OSPL_UNLOCK(_this);
        return V_RESULT_ALREADY_DELETED;
    }
    result = v_dataReaderAccessTest(_this);
    if (result == V_RESULT_OK) {
        _this->readCnt++;
        if (v_readerAccessScope(_this) != V_PRESENTATION_GROUP) {
            v_dataReaderUpdatePurgeLists(_this);
        }
        if (v_orderedInstanceIsAligned (_this->orderedInstance)) {
            result = v__dataReaderOrderedReadOrTake(_this, mask, &v_dataReaderSampleRead, action, arg);
            if (result == V_RESULT_PRECONDITION_NOT_MET) {
                result = V_RESULT_OK;
            } else {
                unordered = FALSE;
            }
        }
        if (unordered) {
            C_STRUCT(readSampleArg) argument;

            argument.reader = _this;
            argument.mask = mask;
            argument.action = action;
            argument.arg = arg;
            argument.query = NULL;
            argument.count = 0;

            while ((argument.count == 0) && (result == V_RESULT_OK))
            {
                argument.emptyList = NULL;
                (void)c_tableReadCircular(v_dataReaderNotEmptyInstanceSet(_this),
                                          (c_action)instanceReadSamples,
                                          &argument);

                /* The state of an instance can also change because of a read action
                 * in case of an invalid sample.
                 */
                if (argument.emptyList != NULL) {
                    emptyInstance = c_iterTakeFirst(argument.emptyList);
                    while (emptyInstance != NULL) {
                        v_dataReaderRemoveInstance(_this,emptyInstance);
                        c_free(emptyInstance);
                        emptyInstance = c_iterTakeFirst(argument.emptyList);
                    }
                    c_iterFree(argument.emptyList);
                    if (_this->statistics) {
                        _this->statistics->numberOfInstances = v_dataReaderInstanceCount_nl(_this);
                    }
                }
                resetCommunicationStatusFlags(_this);
                /* If no data read then wait for data or timeout. */
                if (argument.count == 0) {
                    result = waitForData(_this, &timeout);
                }
            }
        }

        /* Now trigger the action routine that the last sample is read. */
        action(NULL,arg);
        if (_this->statistics) {
            _this->statistics->numberOfReads++;
        }
    }
    OSPL_UNLOCK(_this);

    return result;
}

c_array
v_dataReaderKeyList(
    v_dataReader _this)
{
    assert(_this != NULL);
    assert(C_TYPECHECK(_this, v_dataReader));

    return v_indexKeyList(_this->index);
}

c_array
v_dataReaderSourceKeyList(
    v_dataReader _this)
{
    assert(_this != NULL);
    assert(C_TYPECHECK(_this, v_dataReader));

    return v_indexSourceKeyList(_this->index);
}

v_result
v_dataReaderReadInstance(
    v_dataReader _this,
    v_dataReaderInstance instance,
    v_sampleMask mask,
    v_readerSampleAction action,
    c_voidp arg,
    os_duration timeout)
{
    v_result result = V_RESULT_OK;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this, v_dataReader));

    if (instance == NULL) {
        return V_RESULT_ILL_PARAM;
    }
    if (v_instanceEntity(instance) != _this) {
        OS_REPORT(OS_ERROR, "v_dataReaderReadInstance", V_RESULT_ILL_PARAM,
                  "Bad parameter: Instance handle does not belong to this DataReader");
        return V_RESULT_ILL_PARAM;
    }
    assert(C_TYPECHECK(instance, v_dataReaderInstance));

    OSPL_LOCK(_this);
    if(!v__entityEnabled_nl(v_entity(_this))) {
        OSPL_UNLOCK(_this);
        return V_RESULT_NOT_ENABLED;
    }
    if (v_readerSubscriber(_this) == NULL) {
        OSPL_UNLOCK(_this);
        return V_RESULT_ALREADY_DELETED;
    }
    result = v_dataReaderAccessTest(_this);
    if (result == V_RESULT_OK) {
        C_STRUCT(readSampleArg) argument;

        argument.reader = _this;
        argument.mask = mask;
        argument.action = action;
        argument.arg = arg;
        argument.query = NULL;
        argument.count = 0;

        v_orderedInstanceUnaligned (_this->orderedInstance);

        _this->readCnt++;
        while ((argument.count == 0) && (result == V_RESULT_OK))
        {
            if (!v_dataReaderInstanceEmpty(instance)) {
                v_dataReaderUpdatePurgeLists(_this);
                (void)v_dataReaderInstanceReadSamples(instance,NULL,mask,
                                                      (v_readerSampleAction)instanceSampleAction,
                                                      &argument);
                resetCommunicationStatusFlags(_this);
                if (v_dataReaderInstanceEmpty(instance)) {
                    v_dataReaderRemoveInstance(_this,instance);
                }
            }
            /* If no data read then wait for data or timeout. */
            if (argument.count == 0) {
                result = waitForData(_this, &timeout);
            }
        }
        /* Now trigger the action routine that the last sample is read. */
        action(NULL,arg);
        if (_this->statistics) {
            _this->statistics->numberOfInstanceReads++;
        }
    }
    OSPL_UNLOCK(_this);

    return result;
}

v_result
v_dataReaderReadNextInstance(
    v_dataReader _this,
    v_dataReaderInstance instance,
    v_sampleMask mask,
    v_readerSampleAction action,
    c_voidp arg,
    os_duration timeout)
{
    v_result result = V_RESULT_OK;
    v_dataReaderInstance cur, next;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this, v_dataReader));


    if (instance != NULL && v_instanceEntity(instance) != _this) {
        OS_REPORT(OS_ERROR, "v_dataReaderReadNextInstance", V_RESULT_ILL_PARAM,
                  "Bad parameter: Instance handle does not belong to this DataReader");
        return V_RESULT_ILL_PARAM;
    }
    assert(C_TYPECHECK(instance, v_dataReaderInstance));
    OSPL_LOCK(_this);
    if(!v__entityEnabled_nl(v_entity(_this))) {
        OSPL_UNLOCK(_this);
        return V_RESULT_NOT_ENABLED;
    }
    if (v_readerSubscriber(_this) == NULL) {
        OSPL_UNLOCK(_this);
        return V_RESULT_ALREADY_DELETED;
    }
    result = v_dataReaderAccessTest(_this);
    if (result == V_RESULT_OK) {
        C_STRUCT(readSampleArg) argument;

        argument.reader = _this;
        argument.mask = mask;
        argument.action = action;
        argument.arg = arg;
        argument.query = NULL;
        argument.count = 0;

        v_orderedInstanceUnaligned (_this->orderedInstance);

        _this->readCnt++;

        v_dataReaderUpdatePurgeLists(_this);
        cur = v_dataReaderNextInstance(_this,instance);
        result = V_RESULT_NO_DATA;
        while (cur != NULL && result == V_RESULT_NO_DATA)
        {
            next = v_dataReaderNextInstance(_this,cur);
            if (v_dataReaderInstanceEmpty(cur)) {
                v_dataReaderRemoveInstance(_this,cur);
            } else {
                (void)v_dataReaderInstanceReadSamples(cur, NULL,mask,
                                                      (v_readerSampleAction)instanceSampleAction,
                                                      &argument);
                if (v_dataReaderInstanceEmpty(cur)) {
                    v_dataReaderRemoveInstance(_this,cur);
                }
                /* If no data read then wait for data or timeout.
                 */
                if (argument.count == 0) {
                    if (next == NULL) {
                        /* No data was availabile so block until data becomes available or timeout.
                         */
                        result = waitForData(_this, &timeout);
                        if (result == V_RESULT_OK) {
                            /* After initial read without data, data has arrived before timeout.
                             * so start reading from start again.
                             */
                            next = v_dataReaderNextInstance(_this,instance);
                            result = V_RESULT_NO_DATA;
                        }
                    }
                } else {
                    result = V_RESULT_OK;
                }
            }
            cur = next;
        }
        /* Now trigger the action routine that the last sample is read. */
        action(NULL,arg);
        resetCommunicationStatusFlags(_this);

        if (_this->statistics) {
            _this->statistics->numberOfNextInstanceReads++;
        }
    }
    OSPL_UNLOCK(_this);

    return result;
}

static c_bool
instanceTakeSamples(
    v_dataReaderInstance instance,
    c_voidp arg)
{
    c_bool proceed = TRUE;
    readSampleArg a = (readSampleArg)arg;

    assert(C_TYPECHECK(a->reader, v_dataReader));
    assert(v_dataReader(a->reader)->resourceSampleCount >= 0);

    if (v_dataReaderInstanceEmpty(instance)) {
        if (v_dataReaderInstanceInNotEmptyList(instance)) {
            c_keep(instance);
            a->emptyList = c_iterInsert(a->emptyList,instance);
        }
        return proceed;
    }
    proceed = v_dataReaderInstanceTakeSamples(instance, a->query, a->mask, instanceSampleAction, arg);
    assert(v_dataReader(a->reader)->resourceSampleCount >= 0);

    if (a->reader->statistics) {
            a->reader->statistics->numberOfSamples = (c_ulong) a->reader->resourceSampleCount;
            a->reader->statistics->numberOfSamplesTaken += (c_ulong) a->count;
    }

    /* If the instance is empty and has no active writers, it may be purged */
    if (v_dataReaderInstanceEmpty(instance) && (instance->liveliness == 0)) {
         a->emptyList = c_iterInsert(a->emptyList,c_keep(instance));
    }

    return proceed;
}

static c_bool
only_if_equal (
    c_object found,
    c_object requested,
    c_voidp arg)
{
    *(c_bool *)arg = (found == requested);
    return *(c_bool *)arg;
}

void
v_dataReaderRemoveInstance(
    v_dataReader _this,
    v_dataReaderInstance instance)
{
    c_object instanceSet;
    c_bool equal, doFree;

    assert(C_TYPECHECK(_this, v_dataReader));
    assert(v_dataReaderInstanceEmpty(instance));

    doFree = FALSE;

    if (v_dataReaderInstanceInNotEmptyList(instance)) {
        instanceSet = v_dataReaderNotEmptyInstanceSet(_this);
        equal = FALSE;
        (void)c_remove(instanceSet, instance, only_if_equal, &equal);
        if (equal) {
            v_dataReaderInstanceInNotEmptyList(instance) = FALSE;
            /* A v_publicFree is needed here as well, but due to the
             * fact that the instance actually might be free by that, do
             * it in the end of the routine for subscriber defined keys
             * readers only.
             */
            c_free(instance);
            doFree = TRUE;
        } else {
            /* The instance apparently isn't a member of the
             * NotEmptySet but there is another instance with equal
             * key values.
             * This is unexpected and should be analyzed if this
             * happens.
             * For now the instance is not removed.
             */
            OS_REPORT(OS_WARNING,
                      "v_dataReaderInstanceRemove",V_RESULT_ILL_PARAM,
                      "try removed incorrect instance from NotEmptySet");
        }
    }

    if (!v_reader(_this)->qos->userKey.v.enable) {
          if (v_dataReaderInstanceNoWriters(instance) && (instance->pending == NULL)) {
            instanceSet = v_dataReaderAllInstanceSet(_this);
            equal = FALSE;
            (void)c_remove(instanceSet, instance, only_if_equal, &equal);
            if (equal) {
                v_deadLineInstanceListRemoveInstance(_this->deadLineList,
                                                     v_deadLineInstance(instance));
                /* Remove the instance from the instance-statistics
                 * administration.
                 */
                UPDATE_READER_STATISTICS_REMOVE_INSTANCE(_this, instance);
                instance->purgeInsertionTime = OS_TIMEM_ZERO;
                v_dataReaderInstanceStateSet(instance, L_REMOVED);
                c_free(instance);
                v_dataReaderInstanceFree(instance);
            } else {
                /* The instance apparently isn't a member of the
                 * DataReader but there is another instance with equal
                 * key values.
                 * This is unexpected and should be analyzed if this
                 * happens.
                 * For now the instance is not removed.
                 */
                OS_REPORT(OS_WARNING,
                          "v_dataReaderInstanceRemove",V_RESULT_ILL_PARAM,
                          "try removed incorrect instance");
            }
        }
    } else if(doFree){
        v_dataReaderInstanceFree(instance);
    }
}

v_result
v_dataReaderTake(
    v_dataReader _this,
    v_sampleMask mask,
    v_readerSampleAction action,
    c_voidp arg,
    os_duration timeout)
{
    v_result result = V_RESULT_OK;
    v_dataReaderInstance emptyInstance;
    c_bool unordered = TRUE;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this, v_dataReader));

    OSPL_LOCK(_this);
    if(!v__entityEnabled_nl(v_entity(_this))) {
        OSPL_UNLOCK(_this);
        return V_RESULT_NOT_ENABLED;
    }
    if (v_readerSubscriber(_this) == NULL) {
        OSPL_UNLOCK(_this);
        return V_RESULT_ALREADY_DELETED;
    }
    result = v_dataReaderAccessTest(_this);
    if (result == V_RESULT_OK) {
        _this->readCnt++;
        if (v_readerAccessScope(_this) != V_PRESENTATION_GROUP) {
            v_dataReaderUpdatePurgeLists(_this);
        }
        if (v_orderedInstanceIsAligned (_this->orderedInstance)) {
            result = v__dataReaderOrderedReadOrTake (
                _this, mask, &v_dataReaderSampleTake, action, arg);
            if (result == V_RESULT_PRECONDITION_NOT_MET) {
                result = V_RESULT_OK;
            } else {
                unordered = FALSE;
            }
        }
        if (unordered) {
            C_STRUCT(readSampleArg) argument;

            argument.action = action;
            argument.mask = mask;
            argument.arg = arg;
            argument.query = NULL;
            argument.reader = _this;
            argument.count = 0;

            while ((argument.count == 0) && (result == V_RESULT_OK))
            {
                argument.emptyList = NULL;
                (void)c_tableReadCircular(v_dataReaderNotEmptyInstanceSet(_this),
                                          (c_action)instanceTakeSamples, &argument);
                if (argument.emptyList != NULL) {
                    emptyInstance = c_iterTakeFirst(argument.emptyList);
                    while (emptyInstance != NULL) {
                        v_dataReaderRemoveInstance(_this,emptyInstance);
                        c_free(emptyInstance);
                        emptyInstance = c_iterTakeFirst(argument.emptyList);
                    }
                    c_iterFree(argument.emptyList);
                    if (_this->statistics) {
                        _this->statistics->numberOfInstances = v_dataReaderInstanceCount_nl(_this);
                    }
                }
                resetCommunicationStatusFlags(_this);
                /* If no data read then wait for data or timeout. */
                if (argument.count == 0) {
                    result = waitForData(_this, &timeout);
                }
            }
        }

        /* Now trigger the action routine that the last sample is read. */
        action(NULL,arg);
        if (_this->statistics) {
            _this->statistics->numberOfTakes++;
        }
    }
    OSPL_UNLOCK(_this);

    return result;
}

v_result
v_dataReaderTakeInstance(
    v_dataReader _this,
    v_dataReaderInstance instance,
    v_sampleMask mask,
    v_readerSampleAction action,
    c_voidp arg,
    os_duration timeout)
{
    v_result result = V_RESULT_OK;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this, v_dataReader));

    if (instance == NULL) {
        return V_RESULT_ILL_PARAM;
    }
    if (v_instanceEntity(instance) != _this) {
        OS_REPORT(OS_ERROR, "v_dataReaderTakeInstance", V_RESULT_ILL_PARAM,
                  "Bad parameter: Instance handle does not belong to this DataReader");
        return V_RESULT_ILL_PARAM;
    }
    assert(C_TYPECHECK(instance, v_dataReaderInstance));

    OSPL_LOCK(_this);
    if(!v__entityEnabled_nl(v_entity(_this))) {
        OSPL_UNLOCK(_this);
        return V_RESULT_NOT_ENABLED;
    }
    if (v_readerSubscriber(_this) == NULL) {
        OSPL_UNLOCK(_this);
        return V_RESULT_ALREADY_DELETED;
    }
    result = v_dataReaderAccessTest(_this);
    if (result == V_RESULT_OK) {
        C_STRUCT(readSampleArg) argument;

        argument.reader = _this;
        argument.mask = mask;
        argument.action = action;
        argument.arg = arg;
        argument.query = NULL;
        argument.count = 0;

        v_orderedInstanceUnaligned (_this->orderedInstance);

        _this->readCnt++;
        while ((argument.count == 0) && (result == V_RESULT_OK))
        {
            if (!v_dataReaderInstanceEmpty(instance)) {
                v_dataReaderUpdatePurgeLists(_this);
                (void)v_dataReaderInstanceTakeSamples(instance,NULL,mask,
                                                      (v_readerSampleAction)instanceSampleAction,
                                                      &argument);
                assert(argument.count >= 0);
                if (argument.count > 0) {
                    if (_this->statistics) {
                        _this->statistics->numberOfSamples = (c_ulong) _this->resourceSampleCount;
                    }
                    assert(_this->resourceSampleCount >= 0);
                    resetCommunicationStatusFlags(_this);
                    if (v_dataReaderInstanceEmpty(instance)) {
                        v_dataReaderRemoveInstance(_this,instance);
                    }
                }
            } else {
                v_dataReaderRemoveInstance(_this,instance);
            }

            /* If no data read then wait for data or timeout. */
            if (argument.count == 0) {
                result = waitForData(_this, &timeout);
            }
        }
        /* Now trigger the action routine that the last sample is read. */
        action(NULL,arg);
        if (_this->statistics) {
            _this->statistics->numberOfInstanceTakes++;
        }
    }
    OSPL_UNLOCK(_this);

    return result;
}

v_result
v_dataReaderTakeNextInstance(
    v_dataReader _this,
    v_dataReaderInstance instance,
    v_sampleMask mask,
    v_readerSampleAction action,
    c_voidp arg,
    os_duration timeout)
{
    v_result result = V_RESULT_OK;
    v_dataReaderInstance cur, next;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this, v_dataReader));

    if (instance != NULL && v_instanceEntity(instance) != _this) {
        OS_REPORT(OS_ERROR, "v_dataReaderTakeNextInstance", V_RESULT_ILL_PARAM,
                  "Bad parameter: Instance handle does not belong to this DataReader");
        return V_RESULT_ILL_PARAM;
    }
    assert(C_TYPECHECK(instance, v_dataReaderInstance));
    OSPL_LOCK(_this);
    if(!v__entityEnabled_nl(v_entity(_this))) {
        OSPL_UNLOCK(_this);
        return V_RESULT_NOT_ENABLED;
    }
    if (v_readerSubscriber(_this) == NULL) {
        OSPL_UNLOCK(_this);
        return V_RESULT_ALREADY_DELETED;
    }
    result = v_dataReaderAccessTest(_this);
    if (result == V_RESULT_OK) {
        C_STRUCT(readSampleArg) argument;

        argument.reader = _this;
        argument.mask = mask;
        argument.action = action;
        argument.arg = arg;
        argument.query = NULL;
        argument.count = 0;
        v_orderedInstanceUnaligned (_this->orderedInstance);
        _this->readCnt++;
        v_dataReaderUpdatePurgeLists(_this);
        cur = v_dataReaderNextInstance(_this,instance);
        result = V_RESULT_NO_DATA;
        while (cur != NULL && result == V_RESULT_NO_DATA)
        {
            next = v_dataReaderNextInstance(_this,cur);
            if (v_dataReaderInstanceEmpty(cur)) {
                v_dataReaderRemoveInstance(_this,cur);
            } else {
                (void)v_dataReaderInstanceTakeSamples(cur, NULL, mask,
                                                      (v_readerSampleAction)instanceSampleAction,
                                                      &argument);
                if (v_dataReaderInstanceEmpty(cur)) {
                    v_dataReaderRemoveInstance(_this,cur);
                }
                /* If no data read then wait for data or timeout. */
                if (argument.count == 0) {
                    if (next == NULL) {
                        /* No data was availabile so block until data becomes available or timeout. */
                        result = waitForData(_this, &timeout);
                        if (result == V_RESULT_OK) {
                            /* After initial read without data, data has arrived before timeout.
                             * so start reading from start again.
                             */
                            next = v_dataReaderNextInstance(_this,instance);
                            result = V_RESULT_NO_DATA;
                        }
                    }
                } else {
                    result = V_RESULT_OK;
                }
            }
            cur = next;
        }
        /* Now trigger the action routine that the last sample is read. */
        action(NULL,arg);

        resetCommunicationStatusFlags(_this);

        if (_this->statistics) {
            _this->statistics->numberOfNextInstanceTakes++;
        }
    }
    OSPL_UNLOCK(_this);

    return result;
}

void
v_dataReaderNotify(
    v_dataReader _this,
    v_event event,
    c_voidp userData)
{
    /* This Notify method is part of the observer-observable pattern.
     * It is designed to be invoked when _this object as observer receives
     * an event from an observable object.
     * It must be possible to pass the event to the subclass of itself by
     * calling <subclass>Notify(_this, event, userData).
     * This implies that _this cannot be locked within any Notify method
     * to avoid deadlocks.
     * For consistency _this must be locked by OSPL_LOCK(_this) before
     * calling this method.
     */
    assert(_this != NULL);
    assert(C_TYPECHECK(_this, v_dataReader));
    assert(event != NULL);

    OS_UNUSED_ARG(userData);

#define _NOTIFICATION_MASK_ \
        V_EVENT_SAMPLE_REJECTED | \
        V_EVENT_SAMPLE_LOST | \
        V_EVENT_REQUESTED_DEADLINE_MISSED | \
        V_EVENT_REQUESTED_INCOMPATIBLE_QOS | \
        V_EVENT_SUBSCRIPTION_MATCHED | \
        V_EVENT_LIVELINESS_CHANGED | \
        V_EVENT_DATA_AVAILABLE

    v_entity(_this)->status->state |= (event->kind & (_NOTIFICATION_MASK_));
#undef _NOTIFICATION_MASK_
}

static c_bool
queryNotifyDataAvailable(
    c_object query,
    c_voidp arg)
{
    v_event event = (v_event)arg;

    event->source = v_observable(query);
    v_dataReaderQueryNotifyDataAvailable(v_dataReaderQuery(query),event);

    return TRUE;
}

void
v_dataReaderNotifyDataAvailable(
    v_dataReader _this,
    v_dataReaderSample sample)
{
    C_STRUCT(v_event) event;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this,v_dataReader));
    assert(C_TYPECHECK(sample,v_dataReaderSample));

    EVENT_TRACE("v_dataReaderNotifyDataAvailable(_this = 0x%x, sample = 0x%x)\n", _this, sample);

    v_statusNotifyDataAvailable(v_entity(_this)->status);

    if (_this->triggerValue) {
        v_dataReaderTriggerValueFree(_this->triggerValue);
        _this->triggerValue = NULL;
    }

    if (sample) {
        _this->triggerValue = v_dataReaderTriggerValueKeep(sample);
    }

    /* First send the event to all queries and possibly trigger waitsets,
     * the actual source of the event from the perspective of the waitset
     * is the query so the source can be left out - it will be set to the
     * query in the callback operation, queryNotifyDataAvailable.
     */
    event.kind = V_EVENT_DATA_AVAILABLE;
    event.source = NULL;
    event.data = sample;

    if (v_collection(_this)->queries) {
        c_setWalk(v_collection(_this)->queries, queryNotifyDataAvailable, &event);
    }

    /* Now notify all other observers and listeners.
     * Also notify myself, since the user reader might be waiting.
     */
    event.source = v_observable(_this);

    event.handled = v_entityNotifyListener(v_entity(_this), &event);
    if (!event.handled) {
        if (v_reader(_this)->subscriber) {
            v_subscriberNotifyDataAvailable(v_reader(_this)->subscriber,NULL);
        }
    }
    OSPL_TRIGGER_EVENT(_this, &event, NULL);
    if (!event.handled) {
        OSPL_THROW_EVENT(_this, &event);
    }
}

void
v_dataReaderNotifySampleLost_nl(
    v_dataReader _this,
    c_ulong nrSamplesLost)
{
    C_STRUCT(v_event) event;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this,v_dataReader));

    if (_this->statistics) {
        _this->statistics->numberOfSamplesLost += nrSamplesLost;
    }
    v_statusNotifySampleLost(v_entity(_this)->status, nrSamplesLost);

    /* Also notify myself, since the user reader might be waiting. */
    event.kind = V_EVENT_SAMPLE_LOST;
    event.source = v_observable(_this);
    event.data = NULL;
    event.handled = v_entityNotifyListener(v_entity(_this), &event);
    if (!event.handled) {
        OSPL_THROW_EVENT(_this, &event);
    }
}

void
v_dataReaderNotifySampleLost(
    v_dataReader _this,
    c_ulong nrSamplesLost)
{
    assert(_this != NULL);
    assert(C_TYPECHECK(_this,v_dataReader));

    OSPL_LOCK(_this);
    v_dataReaderNotifySampleLost_nl(_this, nrSamplesLost);
    OSPL_UNLOCK(_this);
}

void
v_dataReaderNotifySampleRejected(
    v_dataReader _this,
    v_sampleRejectedKind kind,
    v_gid instanceHandle)
{
    C_STRUCT(v_event) event;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this,v_reader));

    v_statusNotifySampleRejected(v_entity(_this)->status,
                                           kind,instanceHandle);
    /* Also notify myself, since the user reader might be waiting. */
    event.kind = V_EVENT_SAMPLE_REJECTED;
    event.source = v_observable(_this);
    event.data = NULL;
    event.handled = v_entityNotifyListener(v_entity(_this), &event);
    if (!event.handled) {
        OSPL_THROW_EVENT(_this, &event);
    }
}

void
v_dataReaderNotifyIncompatibleQos(
    v_dataReader _this,
    v_policyId   id,
    v_gid        writerGID)
{
    C_STRUCT(v_event) event;

    OS_UNUSED_ARG(writerGID);
    assert(_this != NULL);
    assert(C_TYPECHECK(_this,v_dataReader));

    OSPL_LOCK(_this);

    v_statusNotifyRequestedIncompatibleQos(v_entity(_this)->status, id);

    /* Also notify myself, since the user reader might be waiting. */
    event.kind = V_EVENT_REQUESTED_INCOMPATIBLE_QOS;
    event.source = v_observable(_this);
    event.data = NULL;
    event.handled = v_entityNotifyListener(v_entity(_this), &event);
    if (!event.handled) {
        OSPL_THROW_EVENT(_this, &event);
    }
    OSPL_UNLOCK(_this);
}

struct updateTransactionsArg {
    v_gid writerGID;
    c_bool dispose;
    struct v_publicationInfo *publicationInfo;
};

void
v_dataReaderNotifySubscriptionMatched (
    v_dataReader _this,
    v_gid        writerGID,
    c_bool       dispose,
    struct v_publicationInfo *publicationInfo,
    c_bool       isImplicit)
{
    C_STRUCT(v_event) event;
    v_subscriber subscriber;
    c_bool flush = FALSE;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this,v_dataReader));

    OSPL_LOCK(_this);
    subscriber = v_readerSubscriber(v_reader(_this));
    if (subscriber) {
        if (v_reader(_this)->subQos->presentation.v.coherent_access) {
            if (_this->entry) {
                flush = v_transactionAdminNotifyPublication(_this->entry->transactionAdmin, writerGID,
                                                            dispose, publicationInfo, isImplicit);
            }
        }
        v_statusNotifySubscriptionMatched(v_entity(_this)->status, writerGID, dispose);
        event.kind = V_EVENT_SUBSCRIPTION_MATCHED;
        event.source = v_observable(_this);
        event.data = NULL;
        event.handled = v_entityNotifyListener(v_entity(_this), &event);
        if (!event.handled) {
            OSPL_THROW_EVENT(_this, &event);
        }

    }
    OSPL_UNLOCK(_this);
    if (flush ) {
        /* Removal of a reader could complete a transaction */
        v_subscriberGroupTransactionFlush(subscriber);
    }
}

/* only called by v_subscriberSetQos() and v_readerSetQos(). */
void
v_dataReaderNotifyChangedQos(
    v_dataReader _this)
{
    v_kernel kernel = NULL;
    v_message builtinMsg = NULL;
    v_message builtinCMMsg = NULL;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this,v_dataReader));

    /* publish subscription info */
    OSPL_LOCK(_this);

    if (v__entityEnabled_nl(v_entity(_this))) {
        kernel = v_objectKernel(_this);
        c_free(_this->subInfo);
        _this->subInfo = v_builtinCreateSubscriptionInfo(kernel->builtin, v_reader(_this));
        builtinMsg = c_keep(_this->subInfo);
        builtinCMMsg = v_builtinCreateCMDataReaderInfo(kernel->builtin, v_reader(_this));
    }

    v_deadLineInstanceListSetDuration(_this->deadLineList, v_reader(_this)->qos->deadline.v.period);
    OSPL_UNLOCK(_this);

    if (builtinMsg) {
        v_writeBuiltinTopic(kernel, V_SUBSCRIPTIONINFO_ID, builtinMsg);
        c_free(builtinMsg);
    }

    if (builtinCMMsg) {
        v_writeBuiltinTopic(kernel, V_CMDATAREADERINFO_ID, builtinCMMsg);
        c_free(builtinCMMsg);
    }
}

void
v_dataReaderNotifyLivelinessChanged(
    v_dataReader _this,
    v_gid wGID,
    enum v_statusLiveliness oldLivState,
    enum v_statusLiveliness newLivState,
    v_message publicationInfo)
{
    OS_UNUSED_ARG(publicationInfo);
    assert(_this != NULL);
    assert(C_TYPECHECK(_this,v_dataReader));
    assert(oldLivState != V_STATUSLIVELINESS_COUNT);
    assert(newLivState != V_STATUSLIVELINESS_COUNT);

    if (oldLivState != newLivState) {
        c_bool genEvent = 1;

        OSPL_LOCK(_this);
        /**
         * Transition table:
         *
         *                          old
         *              UNKNOWN  ALIVE NOTALIVE  DELETED
         *
         *    UNKNOWN      -       -       -        -
         *
         * n  ALIVE        B       -       C        B
         * e
         * w  NOTALIVE     B       C       -        B
         *
         *    DELETED      -       A       A        -
         *
         * Actions:
         * -: No action
         * A: the new state is DELETED, so depending on the old state
         *    the active or inactive must be decremented.
         *    And the writer must be unregistered with this reader.
         * B: the old state is UNKNOWN or DELETED, so this is the first
         *    time we see this writer. Depending on the new state this
         *    writer is alive or not alive to us.
         * C: the writer is already known, only it state has changed
         *    ALIVE -> NOTALIVE or NOTALIVE->ALIVE. In case of the first
         *    transition, the writer must also be unregistered.
         */
        if (newLivState == V_STATUSLIVELINESS_DELETED) { /* A */
            switch(oldLivState) {
            case V_STATUSLIVELINESS_ALIVE:
                v_statusNotifyLivelinessChanged(
                        v_entity(_this)->status, -1, 0, wGID);
            break;
            case V_STATUSLIVELINESS_NOTALIVE:
                v_statusNotifyLivelinessChanged(
                        v_entity(_this)->status, 0, -1, wGID);
            break;
            default: /* no action! */
                genEvent = 0;
            break;
            }
        } else {
            if ((oldLivState == V_STATUSLIVELINESS_UNKNOWN) ||
                (oldLivState == V_STATUSLIVELINESS_DELETED)) { /* B */
                switch(newLivState) {
                case V_STATUSLIVELINESS_ALIVE:
                    v_statusNotifyLivelinessChanged(
                            v_entity(_this)->status, 1, 0, wGID);
                break;
                case V_STATUSLIVELINESS_NOTALIVE:
                    v_statusNotifyLivelinessChanged(
                            v_entity(_this)->status, 0, 1, wGID);

                    if(v_reader(_this)->qos->ownership.v.kind == V_OWNERSHIP_EXCLUSIVE){
                        c_tableWalk(v_dataReaderAllInstanceSet(_this),resetInstanceOwner,&wGID);
                    }
                break;
                default: /* no action! */
                    genEvent = 0;
                break;
                }
            } else {
                if (newLivState != V_STATUSLIVELINESS_UNKNOWN) { /* C: ALIVE or NOTALIVE */
                    assert((newLivState == V_STATUSLIVELINESS_ALIVE) ||
                           (newLivState == V_STATUSLIVELINESS_NOTALIVE));
                    if (newLivState == V_STATUSLIVELINESS_ALIVE) {
                        v_statusNotifyLivelinessChanged(
                                v_entity(_this)->status, 1, -1, wGID);
                    } else {
                        assert(newLivState == V_STATUSLIVELINESS_NOTALIVE);
                        v_statusNotifyLivelinessChanged(
                                v_entity(_this)->status, -1, 1, wGID);

                        if(v_reader(_this)->qos->ownership.v.kind == V_OWNERSHIP_EXCLUSIVE){
                            c_tableWalk(v_dataReaderAllInstanceSet(_this),resetInstanceOwner,&wGID);
                        }
                    }
                } else {
                    genEvent = 0;
                }
            }
        }

        if (genEvent) {
            C_STRUCT(v_event) event;
            event.kind = V_EVENT_LIVELINESS_CHANGED;
            event.source = v_observable(_this);
            event.data = NULL;
            event.handled = v_entityNotifyListener(v_entity(_this), &event);
            if (!event.handled) {
                OSPL_THROW_EVENT(_this, &event);
            }
        }

        OSPL_UNLOCK(_this);
    }
}

static c_bool
v__dataReaderNotifyOwnershipStrengthChangedCallback (
    c_object obj,
    c_voidp arg)
{
    v_dataReaderInstance readerInst = v_dataReaderInstance (obj);
    struct v_owner *candidate = (struct v_owner *)arg;

    assert (candidate != NULL);

    /* First determine whether the writer changing its strength is actually the current owner.
     * If not, then no change is necessary. If it is, and the strength got lowered, then lower
     * the recorded strength as well. Increasing strength will not automatically be recorded:
     * that will happen upon arrival of the first sample with the increased strength.
     */
    if ((v_gidCompare (readerInst->owner.gid, candidate->gid) == C_EQ)
            && readerInst->owner.strength > candidate->strength) {
        readerInst->owner.strength = candidate->strength;
    }

    return TRUE;
}

void
v_dataReaderNotifyOwnershipStrengthChanged (
    v_dataReader _this,
    struct v_owner *ownership)
{
    assert (C_TYPECHECK (_this, v_dataReader));
    assert (ownership != NULL);

    OSPL_LOCK(_this);
    (void)c_tableWalk (
        v_dataReaderAllInstanceSet (_this),
       &v__dataReaderNotifyOwnershipStrengthChangedCallback,
        ownership);
    OSPL_UNLOCK(_this);
}

c_type
v_dataReaderInstanceType(
    v_dataReader _this)
{
    assert(C_TYPECHECK(_this,v_dataReader));
    return c_subType(v_dataReaderAllInstanceSet(_this)); /* pass refCount */
}

c_type
v_dataReaderSampleType(
    v_dataReader _this)
{
    assert(C_TYPECHECK(_this,v_dataReader));
    return c_keep (_this->sampleType);
}

c_char *
v_dataReaderKeyExpr(
    v_dataReader _this)
{
    return v_indexKeyExpr(_this->index);
}

c_field
v_dataReaderIndexField(
    v_dataReader _this,
    const c_char *name)
{
    c_field field;
    c_type  instanceType;
    c_char *fieldName;

    instanceType = v_dataReaderInstanceType(_this);
    /* Try lookup the specified name as an instance field. */
    field = c_fieldNew(instanceType,name);
    if (field == NULL) {
        fieldName = os_alloca(strlen(name) + strlen("sample.message.userData.") + 1);
        /* Try to lookup the specified name as a sample field. */
        os_sprintf(fieldName,"sample.%s",name);
        field = c_fieldNew(instanceType,fieldName);
        if (field == NULL) {
            /* Try to lookup the specified name as a message field. */
            os_sprintf(fieldName,"sample.message.%s",name);
            field = c_fieldNew(instanceType,fieldName);
            if (field == NULL) {
                /* Try to lookup the specified name as a userData field. */
                os_sprintf(fieldName,"sample.message.userData.%s",name);
                field = c_fieldNew(instanceType,fieldName);
            }
        }
        os_freea(fieldName);
    }
    c_free(instanceType);
    return field;
}

c_field
v_dataReaderField(
    v_dataReader _this,
    const c_char *name)
{
    c_field field;

    field = v_projectionSource(_this->projection,name);
    if (field == NULL) {
        field = v_dataReaderIndexField(_this,name);
    } else {
        /* The specified name identified a projection field. */
        /* Need to keep the field because v_projectionSource doesn't. */
        c_keep(field);
    }
    return field;
}

_Check_return_
_Ret_maybenull_
v_topic
v_dataReaderGetTopic(
    v_dataReader _this)
{
    assert(C_TYPECHECK(_this,v_dataReader));
    return c_keep(_this->entry->topic);
}

v_dataReaderInstance
v_dataReaderLookupInstance(
    v_dataReader _this,
    v_message keyTemplate)
{
    v_dataReaderInstance found;

    assert(C_TYPECHECK(_this,v_dataReader));
    assert(C_TYPECHECK(keyTemplate,v_message));

    OSPL_LOCK(_this);
    if(!v__entityEnabled_nl(v_entity(_this))) {
        OSPL_UNLOCK(_this);
        return NULL;
    }
    found = dataReaderLookupInstanceUnlocked(_this, keyTemplate);
    if (_this->statistics) {
        _this->statistics->numberOfInstanceLookups++;
    }
    OSPL_UNLOCK(_this);

    return found;
}

c_bool
v_dataReaderContainsInstance (
    v_dataReader _this,
    v_dataReaderInstance instance)
{
    c_bool result = FALSE;

    assert(C_TYPECHECK(_this,v_dataReader));

    if (instance != NULL) {
        if (v_instanceEntity(instance) == _this) {
            result = TRUE;
        } else {
            OS_REPORT(OS_ERROR, "v_dataReaderContainsInstance", V_RESULT_PRECONDITION_NOT_MET,
                "Invalid dataReaderInstance: no attached DataReader"
                "<_this = 0x%"PA_PRIxADDR" instance = 0x%"PA_PRIxADDR">", (os_address)_this, (os_address)instance);
        }
    }
    return result;
}

void
v_dataReaderCheckDeadlineMissed(
    v_dataReader _this,
    os_timeE now)
{
    C_STRUCT(v_event) event;
    c_iter missed;
    v_dataReaderInstance instance;

    event.kind = V_EVENT_REQUESTED_DEADLINE_MISSED;
    event.source = v_observable(_this);
    event.data = NULL;
    event.handled = TRUE;

    OSPL_LOCK(_this);
    missed = v_deadLineInstanceListCheckDeadlineMissed(_this->deadLineList, v_reader(_this)->qos->deadline.v.period, now);
    instance = v_dataReaderInstance(c_iterTakeFirst(missed));
    while (instance != NULL) {
        if(instance->owner.exclusive){ /*Exclusive ownership*/
            v_gidSetNil(instance->owner.gid);
            instance->owner.strength = 0;
        }
        v_statusNotifyRequestedDeadlineMissed(v_entity(_this)->status,v_publicHandle(v_public(instance)));
        // TODO: event.handled is overwritten, this looks incorect, need a closer look!
        event.handled = v_entityNotifyListener(v_entity(_this), &event);
        instance = v_dataReaderInstance(c_iterTakeFirst(missed));
    }
    c_iterFree(missed);

    if (!event.handled) {
        OSPL_THROW_EVENT(_this, &event);
    }
    OSPL_UNLOCK(_this);
}

v_dataReaderInstance
v_dataReaderAllocInstance(
    v_dataReader _this)
{
    v_dataReaderInstance instance;
    v_kernel kernel;

    kernel = v_objectKernel(_this);

    instance = v_dataReaderInstance(c_new(_this->index->objectType));

    v_object(instance)->kernel = kernel;
    v_objectKind(instance) = K_DATAREADERINSTANCE;

    instance->index = (c_voidp)_this->index;

    return instance;
}

c_long
v_dataReaderNotReadCount(
    v_dataReader _this)
{
    c_long count;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this,v_dataReader));

    OSPL_LOCK(_this);
    count = _this->notReadCount;
    OSPL_UNLOCK(_this);

    return count;
}

static v_actionResult
v__dataReaderFindMatchingSample(
    c_object object,
    c_voidp data)
{
    v_actionResult walkon = V_PROCEED;
    v_dataReaderSample sample;
    v_sampleMask *maskptr = (v_sampleMask *)data;

    assert (object != NULL && C_TYPECHECK (object, v_dataReaderSample));
    assert (data != NULL);

    /* Instance state already verified and V_MASK_ANY_SAMPLE was not set. */

    sample = v_dataReaderSample (object);
    if (v_readerSampleTestStateOr (sample, L_READ | L_LAZYREAD)) {
        if (v_sampleMaskTest (*maskptr, V_MASK_READ_SAMPLE)) {
            walkon = V_STOP; /* Reader qualifies, stop iteration */
        }
    } else {
        if (v_sampleMaskTest (*maskptr, V_MASK_NOT_READ_SAMPLE)) {
            walkon = V_STOP; /* Reader qualifies, stop iteration */
        }
    }

    return walkon;
}

static c_bool
v__dataReaderFindMatchingInstance(
    v_dataReaderInstance instance,
    c_voidp data)
{
    c_bool walkon = TRUE;
    v_sampleMask mask = V_MASK_ALIVE_INSTANCE;
    v_sampleMask *maskptr = (v_sampleMask *)data;

    assert (instance != NULL && C_TYPECHECK (instance, v_dataReaderInstance));
    assert (data != NULL);

    if (v_stateTest (v_dataReaderInstanceState (instance), L_NEW)) {
        v_sampleMaskSet (mask, V_MASK_NEW_VIEW);
    } else {
        v_sampleMaskSet (mask, V_MASK_NOT_NEW_VIEW);
    }

    if (v_stateTest (v_dataReaderInstanceState (instance), L_DISPOSED)) {
        v_sampleMaskSet (mask, V_MASK_DISPOSED_INSTANCE);
        v_sampleMaskClear (mask, V_MASK_ALIVE_INSTANCE);
    }
    if (v_stateTest (v_dataReaderInstanceState (instance), L_NOWRITERS)) {
        v_sampleMaskSet (mask, V_MASK_NOWRITERS_INSTANCE);
        v_sampleMaskClear (mask, V_MASK_ALIVE_INSTANCE);
    }

    if (v_sampleMaskTest (*maskptr, mask) && instance->historySampleCount > 0) {
        if ((v_sampleMaskTest (*maskptr, V_MASK_ANY_SAMPLE)) ||
            (v_sampleMaskTest (*maskptr, V_MASK_NOT_READ_SAMPLE) &&
                (v_sampleMaskTest (mask, V_MASK_NEW_VIEW))))
        {
            /* User is either only interested in instance states or required
             * sample state is not read and instance is marked as new, which
             * indicates there are samples in the instance and that are not yet
             * readread.
             */
            walkon = FALSE; /* Reader qualifies, stop iteration. */
        } else {
            walkon = V_STOP != v_dataReaderInstanceWalkSamples (
                instance, &v__dataReaderFindMatchingSample, data);
        }
    }

    return walkon;
}

c_bool
v_dataReaderHasMatchingSamples(
    v_dataReader _this,
    v_sampleMask mask)
{
    c_bool match = FALSE;

    assert (_this != NULL && C_TYPECHECK (_this, v_dataReader));

    OSPL_LOCK(_this);
    if (v_sampleMaskTest(mask, V_MASK_NOT_READ_SAMPLE) && _this->notReadCount) {
        assert (_this->notReadCount > 0); /* notReadCount is signed */

        if (v_sampleMaskTest(mask, V_MASK_ANY_VIEW | V_MASK_ANY_INSTANCE)) {
            match = TRUE;
        } else {
            /* User is either only interested in instance states or instance
             * is required to be NEW, which indicates it's samples have not
             * been read.
             * If iteration ends prematurely at least one matching instance
             * and/or sample was identified. Iteration is terminated by
             * returning FALSE.
             */
            match = FALSE == v__dataReaderWalkInstances (
                _this, &v__dataReaderFindMatchingInstance, (c_voidp)&mask);
        }
    } else if (v_sampleMaskTest (mask, V_MASK_READ_SAMPLE)) {
        match = FALSE == v__dataReaderWalkInstances (
            _this, &v__dataReaderFindMatchingInstance, (c_voidp)&mask);
    }
    OSPL_UNLOCK(_this);

    return match;
}

static c_bool
countHistorySamples(
    c_object o,
    c_voidp arg)
{
    v_dataReaderInstance instance = (v_dataReaderInstance)o;
    c_long *count = (c_long *)arg;

    *count += instance->historySampleCount;
    return TRUE;
}

c_long
v_dataReaderHistoryCount(
    v_dataReader _this)
{
    c_long count = 0;
    assert (_this != NULL && C_TYPECHECK (_this, v_dataReader));

    (void) c_tableWalk(v_dataReaderNotEmptyInstanceSet(_this), countHistorySamples, &count); /* Always returns TRUE. */
    return count;
}

static c_bool
countTransactions (
    c_object o,
    c_voidp arg)
{
    c_ulong *count = arg;

    OS_UNUSED_ARG(o);

    (*count)++;

    return TRUE;
}

c_ulong
v_dataReaderGetNumberOpenTransactions(
    v_dataReader _this)
{
    c_ulong count = 0;

    assert (_this != NULL && C_TYPECHECK (_this, v_dataReader));

    if(!v_entityEnabled(v_entity(_this))) {
        return 0;
    }
    OSPL_LOCK(_this);
    if (_this->entry->transactionAdmin) {
        v_transactionAdminWalkTransactions(_this->entry->transactionAdmin, countTransactions, &count);
    }
    OSPL_UNLOCK(_this);
    return count;
}

/* This operation is called by the leaseManager because the lease on samples within the
 * readers minimum separation window has expired meaning that they must be made available.
 * This function will walk over all registered instances (those that have pending samples) and
 * make the samples for those instances whose last insertion time is older than the minimum
 * separation time available.
 */
void
v_dataReaderCheckMinimumSeparationList(
    v_dataReader _this,
    os_timeE now)
{
    v_dataReaderInstance instance;
    c_bool processed;
    c_list reinsertList = NULL;
    os_timeE lastInsertionTime = now;

    OSPL_LOCK(_this);
    while ((instance = c_take(_this->minimumSeparationList))) {
        processed = v_dataReaderInstanceCheckMinimumSeparation(instance, now);
        if (!processed) {
            /* The maximum separation time for this instance was not expired so no samples are made visible yet.
             * Therefore reinsert this instance in the list.
             */
            if (reinsertList == NULL) {
                v_kernel kernel = v_objectKernel(_this);
                reinsertList = c_listNew(v_kernelType(kernel, K_DATAREADERINSTANCE));
                lastInsertionTime = instance->lastInsertionTime;
            }
            c_append(reinsertList, instance);
        } else {
            /* The sample within the minimum separation window is made available because
             * the maximum separation time has expired for this instance.
             */
            v_dataReaderInstanceStateClear(instance, L_INMINSEPTIME);
        }
        c_free(instance);
    }
    c_free(_this->minimumSeparationList);
    _this->minimumSeparationList = reinsertList;
    if (reinsertList) {
        /* There are still instances with samples in the minimum separation window so therefore
         * get the first instance's last insertion time and calculate the new lease expiry time.
         */
        os_duration d = _this->maximumSeparationTime - os_timeEDiff(now, lastInsertionTime);
        v_leaseRenew(_this->minimumSeparationLease, d);
    } else {
        /* There are no more instances with pending samples so set the lease expiry time to infinite. */
        v_leaseRenew(_this->minimumSeparationLease, OS_DURATION_INFINITE);
    }
    /* replace old list with the updated list. */
    OSPL_UNLOCK(_this);
}

/* This operation will add an instance that has a sample in the minimum separation window to the
 * reader's minimum separation list if not already registered by a previous pending sample.
 */
void
v_dataReaderMinimumSeparationListRegister(
    v_dataReader _this,
    v_dataReaderSample sample)
{
    v_kernel kernel = v_objectKernel(_this);
    v_dataReaderInstance instance = v_dataReaderSampleInstance(sample);
    v_message message = v_dataReaderSampleMessage(sample);

    /* Don't register instances that are already in the list i.e. have the L_INMINSEPTIME flag set. */
    if (!v_dataReaderInstanceStateTest(instance, L_INMINSEPTIME)) {
        if (_this->minimumSeparationList == NULL) {
            /* Lazy create a minimumSeparationList if it doesn't exist. */
            _this->minimumSeparationList = c_listNew(v_kernelType(kernel, K_DATAREADERINSTANCE));
        }
        if (_this->minimumSeparationLease == NULL) {
            /* Lazy create a minimumSeparationLease if it doesn't exist.
             * Set the lease time to maximumSeparationTime for the first instance in the list.
             */
            v_subscriber subscriber = v_readerSubscriber(v_reader(_this));
            v_participant participant = v_subscriberParticipant(subscriber);
            v_result res;
            os_timeE allocTime = message->allocTime;
            os_duration d = _this->maximumSeparationTime - os_timeEDiff(allocTime, instance->lastInsertionTime);
            _this->minimumSeparationLease = v_leaseElapsedNew(kernel, d);
            res = v_leaseManagerRegister( participant->leaseManager, _this->minimumSeparationLease,
                                         V_LEASEACTION_MINIMUM_SEPARATION_EXPIRY, v_public(_this), TRUE);
            if(res != V_RESULT_OK) {
                c_free(_this->minimumSeparationLease);
                _this->minimumSeparationLease = NULL;
                OS_REPORT(OS_CRITICAL, "v_dataReaderMinimumSeparationListRegister", res,
                          "A fatal error was detected when trying to register the minimumSeparationLease."
                          "The result code was %d.", res);
                assert(FALSE);
            }
        } else if (c_count(_this->minimumSeparationList) == 0) {
            /* Lease already exists (the lease duration should be infinity because no instances are registered).
             * Set the lease time to maximumSeparationTime for the first instance in the list.
             */
            os_timeE allocTime = message->allocTime;
            os_duration d = _this->maximumSeparationTime - os_timeEDiff(allocTime, instance->lastInsertionTime);
            v_leaseRenew(_this->minimumSeparationLease, d);
        } else {
            /* Lease already exists (the lease is set to expire for existing instances in the list).
             * So don't renew the lease.
             */
        }
        /* Set the instance L_INMINSEPTIME flag and add the instance to the minimumSeparationList. */
        v_dataReaderInstanceStateSet(instance, L_INMINSEPTIME);
        c_append(_this->minimumSeparationList, instance);
    }
}

/* This operation will remove an instance from the reader's minimum separation list. */
void
v_dataReaderMinimumSeparationListRemove(
    v_dataReader _this,
    v_dataReaderInstance instance)
{
    v_dataReaderInstance found;
    if (_this->minimumSeparationList && v_dataReaderInstanceStateTest(instance, L_INMINSEPTIME)) {
        found = c_remove(_this->minimumSeparationList, instance, NULL, NULL);
        assert(found == instance);
        OS_UNUSED_ARG(found);
        v_dataReaderInstanceStateClear(instance, L_INMINSEPTIME);
        c_free(instance);
    }
}

v_result
v_dataReaderAccessTest(
    v_dataReader _this)
{
    v_result result = V_RESULT_OK;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this, v_dataReader));

    if (!_this->accessBusy &&
        (v_reader(_this)->subQos->presentation.v.coherent_access ||
         v_reader(_this)->subQos->presentation.v.ordered_access) &&
        v_reader(_this)->subQos->presentation.v.access_scope == V_PRESENTATION_GROUP)
    {
        result = V_RESULT_PRECONDITION_NOT_MET;
    }
    return result;
}

v_result
v_dataReaderSetQos(
    v_dataReader _this,
    v_readerQos qos)
{
    v_result result = V_RESULT_OK;

    assert(_this != NULL);
    assert(C_TYPECHECK(_this, v_dataReader));

    result = v_readerSetQos(v_reader(_this), qos);
    OSPL_LOCK(_this);
    if (result == V_RESULT_OK) {
        v_deadLineInstanceListSetDuration(_this->deadLineList, qos->deadline.v.period);
    }
    OSPL_UNLOCK(_this);
    return result;
}

/* This operation informs the reader to either add or remove the publication described by
 * the given publication info depending on the given ignore flag.
 * If ignore is TRUE then the publication will be added to the readers ignore list and if
 * ignore is FALSE then the publication will be removed from the ignore list if it exists.
 * Note that removing a publication from the ignore list should only be performed if no data
 * from the publication is to be expected in the future according to the DCPS specification.
 */
v_result
v_dataReaderIgnore(
    v_dataReader _this,
    struct v_publicationInfo *info,
    os_boolean ignore)
{
    OSPL_LOCK(_this);
    if (ignore) {
        v_dataReaderEntryIgnorePublication(_this->entry, info);
    } else {
        v_dataReaderEntryDisposePublication(_this->entry, info);
    }
    OSPL_UNLOCK(_this);
    return V_RESULT_OK;
}

/* This operation will return the actual builtin subscription data for this dataReader.
 * The dataReader caches this last published subscription data.
 */
v_message
v_dataReaderSubscription(
    v_dataReader _this)
{
    v_message subscription;
    OSPL_LOCK(_this);
    if (_this->subInfo == NULL) {
        _this->subInfo = v_builtinCreateSubscriptionInfo(v_objectKernel(_this)->builtin, v_reader(_this));
    }
    subscription = c_keep(_this->subInfo);
    OSPL_UNLOCK(_this);
    return subscription;
}

struct matchingPublicationsArg {
    struct v_subscriptionInfo *subInfo;
    v_publicationInfo_action action;
    void *arg;
};

static os_boolean
actionOnMatch(
    v_message publication,
    void *arg)
{
    struct matchingPublicationsArg *a = (struct matchingPublicationsArg *)arg;
    struct v_publicationInfo *pubInfo = (struct v_publicationInfo *)(publication + 1);

    if ((strcmp(pubInfo->topic_name, a->subInfo->topic_name) == 0) &&
        v_builtinTestPartitionMatch(pubInfo, a->subInfo) &&
        v_builtinTestQosMatch(pubInfo, a->subInfo) )
    {
        a->action(pubInfo, a->arg);
    }
    return OS_TRUE;
}

/* This operation will visit all discovered matching publications for this dataReader.
 * The given action routine will be invoked on each publication info message.
 * The signature of the action routine : v_result (*action)(const v_publicationInfo *info, void *arg)
 * Issue: don't like operating on info as being an attribute of a message,
 *        better visit the whole message then it can also be returned as kept ref.
 */
v_result
v_dataReaderReadMatchedPublications(
    v_dataReader _this,
    v_publicationInfo_action action,
    c_voidp arg)
{
    v_message msg;
    v_result result = V_RESULT_OK;
    struct matchingPublicationsArg ctx;

    msg = v_dataReaderSubscription(_this);
    if (msg) {
        ctx.subInfo = (struct v_subscriptionInfo *)(msg + 1);
        ctx.action = action;
        ctx.arg = arg;
        result = v_kernelWalkPublications(v_objectKernel(_this), actionOnMatch, &ctx);
        c_free(msg);
    }

    return result;
}

struct readMatchedDataArg {
    struct v_subscriptionInfo *subInfo;
    v_publicationInfo_action action;
    void *arg;
    v_gid publication;
};

static os_boolean
readMatchedData(
    v_message publication,
    void *arg)
{
    os_boolean proceed = OS_TRUE;
    struct readMatchedDataArg *a = (struct readMatchedDataArg *)arg;
    struct v_publicationInfo *pubInfo = (struct v_publicationInfo *)(publication + 1);

    if (pubInfo->key.systemId == a->publication.systemId &&
        pubInfo->key.localId == a->publication.localId)
    {
        if ((strcmp(pubInfo->topic_name, a->subInfo->topic_name) == 0) &&
            v_builtinTestPartitionMatch(pubInfo, a->subInfo) &&
            v_builtinTestQosMatch(pubInfo, a->subInfo) )
        {
            a->action(pubInfo, a->arg);
        }
        proceed = OS_FALSE;
    }
    return proceed;
}

/* This operation will visit the discovered matching publication for this dataReader identified by the given GID.
 * The given action routine will be invoked on the GID associated publication info message.
 * The signature of the action routine : v_result (*action)(const v_publicationInfo *info, void *arg)
 * Issue: don't like operating on info as being an attribute of a message,
 *        better visit the whole message then it can also be returned as kept ref.
 */
v_result
v_dataReaderReadMatchedPublicationData(
    v_dataReader _this,
    v_gid publication,
    v_publicationInfo_action action,
    c_voidp arg)
{
    v_message msg;
    v_result result = V_RESULT_OK;
    struct readMatchedDataArg ctx;

    msg = v_dataReaderSubscription(_this);
    if (msg) {
        ctx.subInfo = (struct v_subscriptionInfo *)(msg + 1);
        ctx.action = action;
        ctx.arg = arg;
        ctx.publication = publication;
        result = v_kernelWalkPublications(v_objectKernel(_this), readMatchedData, &ctx);
        c_free(msg);
    }
    return result;
}
