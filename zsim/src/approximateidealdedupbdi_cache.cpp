#include "approximateidealdedupbdi_cache.h"
#include "pin.H"

ApproximateIdealDedupBDICache::ApproximateIdealDedupBDICache(uint32_t _numTagLines, uint32_t _numDataLines, CC* _cc, ApproximateDedupBDITagArray* _tagArray, ApproximateDedupBDIDataArray* _dataArray, ApproximateDedupBDIHashArray* _hashArray, ReplPolicy* tagRP,
ReplPolicy* dataRP, ReplPolicy* hashRP, uint32_t _accLat, uint32_t _invLat, uint32_t mshrs, uint32_t ways, uint32_t cands, uint32_t _domain, const g_string& _name, RunningStats* _crStats,
RunningStats* _evStats, RunningStats* _tutStats, RunningStats* _dutStats, Counter* _tag_hits, Counter* _tag_misses, Counter* _all_misses) : TimingCache(_numTagLines, _cc, NULL, tagRP, _accLat, _invLat, mshrs, tagLat, ways, cands, _domain, _name, _evStats, _tag_hits, _tag_misses, _all_misses), numTagLines(_numTagLines),
numDataLines(_numDataLines), dataAssoc(ways), tagArray(_tagArray), dataArray(_dataArray), hashArray(_hashArray), tagRP(tagRP), dataRP(dataRP), hashRP(hashRP), crStats(_crStats), evStats(_evStats), tutStats(_tutStats), dutStats(_dutStats) {
    dataArray->assignTagArray(tagArray);
    TM_DS = 0;
    TM_DD = 0;
    WD_TH_DS = 0;
    WD_TH_DD_1 = 0;
    WD_TH_DD_M = 0;
    WSR_TH = 0;
    g_string statName = name + g_string(" Deduplication Average");
    dupStats = new RunningStats(statName);
    statName = name + g_string(" Data Size Average");
    bdiStats = new RunningStats(statName);
}

void ApproximateIdealDedupBDICache::initStats(AggregateStat* parentStat) {
    AggregateStat* cacheStat = new AggregateStat();
    cacheStat->init(name.c_str(), "Approximate BDI cache stats");
    initCacheStats(cacheStat);

    //Stats specific to timing cacheStat
    profOccHist.init("occHist", "Occupancy MSHR cycle histogram", numMSHRs+1);
    cacheStat->append(&profOccHist);

    profHitLat.init("latHit", "Cumulative latency accesses that hit (demand and non-demand)");
    profMissRespLat.init("latMissResp", "Cumulative latency for miss start to response");
    profMissLat.init("latMiss", "Cumulative latency for miss start to finish (free MSHR)");

    cacheStat->append(&profHitLat);
    cacheStat->append(&profMissRespLat);
    cacheStat->append(&profMissLat);

    parentStat->append(cacheStat);
}

void ApproximateIdealDedupBDICache::initCacheStats(AggregateStat* cacheStat) {
    cc->initStats(cacheStat);
    tagArray->initStats(cacheStat);
    tagRP->initStats(cacheStat);
    dataArray->initStats(cacheStat);
    // dataRP->initStats(cacheStat);
    hashRP->initStats(cacheStat);
}

uint64_t ApproximateIdealDedupBDICache::access(MemReq& req) {
    if (tag_all) tag_all->inc();
    DataLine data = gm_calloc<uint8_t>(zinfo->lineSize);
    DataType type = ZSIM_FLOAT;
    bool approximate = false;
    uint64_t Evictions = 0;
    uint64_t readAddress = req.lineAddr;
    if (zinfo->realAddresses->find(req.lineAddr) != zinfo->realAddresses->end())
        readAddress = (*zinfo->realAddresses)[req.lineAddr];
    for(uint32_t i = 0; i < zinfo->approximateRegions->size(); i++) {
        if ((readAddress << lineBits) >= std::get<0>((*zinfo->approximateRegions)[i]) && (readAddress << lineBits) <= std::get<1>((*zinfo->approximateRegions)[i])
        && (readAddress << lineBits)+zinfo->lineSize-1 >= std::get<0>((*zinfo->approximateRegions)[i]) && (readAddress << lineBits)+zinfo->lineSize-1 <= std::get<1>((*zinfo->approximateRegions)[i])) {
            type = std::get<2>((*zinfo->approximateRegions)[i]);
            approximate = true;
            break;
        }
    }
    PIN_SafeCopy(data, (void*)(readAddress << lineBits), zinfo->lineSize);
    // // // info("\tData type: %s, Data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);

    EventRecorder* evRec = zinfo->eventRecorders[req.srcId];
    assert_msg(evRec, "ApproximateBDI is not connected to TimingCore");

    // Tie two events to an optional timing record
    // TODO: Promote to evRec if this is more generally useful
    auto connect = [evRec](const TimingRecord* r, TimingEvent* startEv, TimingEvent* endEv, uint64_t startCycle, uint64_t endCycle) {
        assert_msg(startCycle <= endCycle, "start > end? %ld %ld", startCycle, endCycle);
        if (r) {
            assert_msg(startCycle <= r->reqCycle, "%ld / %ld", startCycle, r->reqCycle);
            assert_msg(r->respCycle <= endCycle, "%ld %ld %ld %ld", startCycle, r->reqCycle, r->respCycle, endCycle);
            uint64_t upLat = r->reqCycle - startCycle;
            uint64_t downLat = endCycle - r->respCycle;

            if (upLat) {
                DelayEvent* dUp = new (evRec) DelayEvent(upLat);
                // // // info("uCREATE: %p at %u", dUp, __LINE__);
                dUp->setMinStartCycle(startCycle);
                startEv->addChild(dUp, evRec)->addChild(r->startEvent, evRec);
            } else {
                startEv->addChild(r->startEvent, evRec);
            }

            if (downLat) {
                DelayEvent* dDown = new (evRec) DelayEvent(downLat);
                // // // info("uCREATE: %p at %u", dDown, __LINE__);
                dDown->setMinStartCycle(r->respCycle);
                r->endEvent->addChild(dDown, evRec)->addChild(endEv, evRec);
            } else {
                r->endEvent->addChild(endEv, evRec);
            }
        } else {
            if (startCycle == endCycle) {
                startEv->addChild(endEv, evRec);
            } else {
                DelayEvent* dEv = new (evRec) DelayEvent(endCycle - startCycle);
                // // // info("uCREATE: %p at %u", dEv, __LINE__);
                dEv->setMinStartCycle(startCycle);
                startEv->addChild(dEv, evRec)->addChild(endEv, evRec);
            }
        }
    };

    TimingRecord tagWritebackRecord, accessRecord, tr;
    tagWritebackRecord.clear();
    accessRecord.clear();
    g_vector<TimingRecord> writebackRecords;
    g_vector<uint64_t> wbStartCycles;
    g_vector<uint64_t> wbEndCycles;
    uint64_t tagEvDoneCycle = 0;
    uint64_t respCycle = req.cycle;
    uint64_t evictCycle = req.cycle;

    // g_vector<uint32_t> keptFromEvictions;

    bool skipAccess = cc->startAccess(req); //may need to skip access due to races (NOTE: may change req.type!)
    if (likely(!skipAccess)) {
        // info("%lu: REQ %s to address %lu in %s region", req.cycle, AccessTypeName(req.type), req.lineAddr << lineBits, approximate? "approximate":"exact");
        // info("Req data type: %s, data: %f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", DataTypeName(type), ((float*)data)[0], ((float*)data)[1], ((float*)data)[2], ((float*)data)[3], ((float*)data)[4], ((float*)data)[5], ((float*)data)[6], ((float*)data)[7], ((float*)data)[8], ((float*)data)[9], ((float*)data)[10], ((float*)data)[11], ((float*)data)[12], ((float*)data)[13], ((float*)data)[14], ((float*)data)[15]);
        bool updateReplacement = (req.type == GETS) || (req.type == GETX);
        int32_t tagId = tagArray->lookup(req.lineAddr, &req, updateReplacement);
        zinfo->tagAll++;
        respCycle += accLat;
        evictCycle += accLat;

        MissStartEvent* mse;
        MissResponseEvent* mre;
        MissWritebackEvent* mwe;
        if (tagId == -1) {
            if(tag_misses) tag_misses->inc();
            zinfo->tagMisses++;
            // info("\tTag Miss");
            assert(cc->shouldAllocate(req));
            // Get the eviction candidate
            Address wbLineAddr;
            int32_t victimTagId = tagArray->preinsert(req.lineAddr, &req, &wbLineAddr); //find the lineId to replace
            // info("\t\tEvicting tagId: %i", victimTagId);
            // keptFromEvictions.push_back(victimTagId);
            trace(Cache, "[%s] Evicting 0x%lx", name.c_str(), wbLineAddr);
            // Need to evict the tag.
            tagEvDoneCycle = cc->processEviction(req, wbLineAddr, victimTagId, evictCycle);
            // // // // info("\t\t\tEviction finished at %lu", tagEvDoneCycle);
            int32_t newLLHead;
            bool evictDataLine = tagArray->evictAssociatedData(victimTagId, &newLLHead);
            int32_t victimDataId = tagArray->readDataId(victimTagId);
            int32_t victimSegmentId = tagArray->readSegmentPointer(victimTagId);
            if (evictDataLine) {
                // info("\t\tAlong with dataId,segmenId of size %i segments: %i, %i", BDICompressionToSize(tagArray->readCompressionEncoding(victimTagId), zinfo->lineSize)/8, victimDataId, victimSegmentId);
                // Clear (Evict, Tags already evicted) data line
                dataArray->postinsert(-1, &req, 0, victimDataId, victimSegmentId, NULL, false);
                // // // info("SHOULD DOWN");
            } else if (newLLHead != -1) {
                // Change Tag
                // info("\t\tAnd decremented tag counter and decremented LL Head for dataId, SegmentId %i, %i", victimDataId, victimSegmentId);
                uint32_t victimCounter = dataArray->readCounter(victimDataId, victimSegmentId);
                dataArray->changeInPlace(newLLHead, &req, victimCounter-1, victimDataId, victimSegmentId, NULL, false);
                // // // info("SHOULDN'T1");
            } else if (victimDataId != -1 && victimSegmentId != -1) {
                // info("\t\tAnd decremented dedup counter for dataId, segmentId %i, %i.", victimDataId, victimSegmentId);
                // // // info("SHOULDN'T2");
                uint32_t victimCounter = dataArray->readCounter(victimDataId, victimSegmentId);
                int32_t LLHead = dataArray->readListHead(victimDataId, victimSegmentId);
                dataArray->changeInPlace(LLHead, &req, victimCounter-1, victimDataId, victimSegmentId, NULL, false);
            }
            tagArray->postinsert(0, &req, victimTagId, -1, -1, NONE, -1, false);
            if (evRec->hasRecord()) {
                // // info("\t\tEvicting tagId: %i", victimTagId);
                Evictions++;
                tagWritebackRecord.clear();
                tagWritebackRecord = evRec->popRecord();
            }

            // Need to get the line we want
            uint64_t getDoneCycle = respCycle;
            respCycle = cc->processAccess(req, victimTagId, respCycle, &getDoneCycle);
            tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
            if (evRec->hasRecord()) accessRecord = evRec->popRecord();

            if(approximate)
                hashArray->approximate(data, type);
            int32_t dataId = -1;
            int32_t segmentId = -1;
            for (uint32_t i = 0; i < numDataLines/dataAssoc; i++) {
                for (uint32_t j = 0; j < dataAssoc*8; j++) {
                    if (dataArray->readCounter(i, j) && dataArray->isSame(i, j, data)) {
                        dataId = i;
                        segmentId = j;
                        break;
                    }
                }
            }
            uint16_t lineSize = 0;
            BDICompressionEncoding encoding = dataArray->compress(data, &lineSize);
            // info("\tMiss Data Size: %u Segments", lineSize/8);
            // // info("size: %i", BDICompressionToSize(encoding, zinfo->lineSize)/8);

            if (dataId != -1) {
                // // info("Found a matching hash, proceeding to match the full line.");
                TM_DS++;
                // info("\t\tfound matching data at %i.", dataId);
                // // info("Data is also similar.");
                int32_t oldListHead = dataArray->readListHead(dataId, segmentId);
                uint32_t dataCounter = dataArray->readCounter(dataId, segmentId);
                // // // info("SHOULDN'T");
                tagArray->postinsert(req.lineAddr, &req, victimTagId, dataId, segmentId, encoding, oldListHead, true);
                // // info("postinsert %i", victimTagId);
                dataArray->changeInPlace(victimTagId, &req, dataCounter+1, dataId, segmentId, NULL, updateReplacement);

                assert_msg(getDoneCycle == respCycle, "gdc %ld rc %ld", getDoneCycle, respCycle);

                mse = new (evRec) MissStartEvent(this, accLat, domain);
                // // // info("uCREATE: %p at %u", mse, __LINE__);
                mre = new (evRec) MissResponseEvent(this, mse, domain);
                // // // info("uCREATE: %p at %u", mre, __LINE__);
                mwe = new (evRec) MissWritebackEvent(this, mse, accLat, domain);
                // // // info("uCREATE: %p at %u", mwe, __LINE__);

                mse->setMinStartCycle(req.cycle);
                // // // info("\t\t\tMiss Start Event: %lu, %u", req.cycle, accLat);
                mre->setMinStartCycle(respCycle);
                // // // info("\t\t\tMiss Response Event: %lu", respCycle);
                mwe->setMinStartCycle(MAX(respCycle, tagEvDoneCycle));
                // // // info("\t\t\tMiss writeback event: %lu, %u", MAX(respCycle, tagEvDoneCycle), accLat);

                connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + accLat, respCycle);
                mre->addChild(mwe, evRec);
                if (tagEvDoneCycle) {
                    connect(tagWritebackRecord.isValid()? &tagWritebackRecord : nullptr, mse, mwe, req.cycle + accLat, tagEvDoneCycle);
                }
            } else {
                TM_DD++;
                // info("\t\tCouldn't find matching hash.");
                // // info("Hash is different, nothing similar.");
                // Select data to evict
                evictCycle = respCycle + accLat;
                int32_t victimDataId = dataArray->preinsert(lineSize);

                // Now we need to know the available space in this set.
                uint16_t freeSpace = 0;
                g_vector<uint32_t> keptFromEvictions;
                uint64_t lastEvDoneCycle = evictCycle;
                uint64_t evBeginCycle = evictCycle;
                do {
                    uint16_t occupiedSpace = 0;
                    for (uint32_t i = 0; i < dataArray->getAssoc()*zinfo->lineSize/8; i++)
                        if (dataArray->readListHead(victimDataId, i) != -1)
                            occupiedSpace += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, i)), zinfo->lineSize);
                    freeSpace = dataArray->getAssoc()*zinfo->lineSize - occupiedSpace;
                    // info("\t\tFree Space %i segments", freeSpace/8);
                    // // info("Free %i, lineSize %i", freeSpace, lineSize);
                    int32_t victimListHeadId, newVictimListHeadId;
                    int32_t victimSegmentId = dataArray->preinsert(victimDataId, &victimListHeadId, keptFromEvictions);
                    // uint32_t size = 0;
                    if (dataArray->readListHead(victimDataId, victimSegmentId) != -1) {
                        freeSpace += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, victimSegmentId)), zinfo->lineSize);
                        // size = BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, victimSegmentId)), zinfo->lineSize)/8;
                    }
                    // info("\t\tEvicting dataline %i,%i", victimDataId, victimSegmentId);

                    keptFromEvictions.push_back(victimSegmentId);
                    uint64_t evDoneCycle = evBeginCycle;
                    TimingRecord writebackRecord;
                    lastEvDoneCycle = tagEvDoneCycle;
                    if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                    while (victimListHeadId != -1) {
                        if (victimListHeadId != victimTagId) {
                            // info("\t\tEvicting TagId: %i", victimListHeadId);
                            Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                            // // info("\t\tEvicting tagId: %i, %lu", victimListHeadId, wbLineAddr);
                            evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                            // // // info("\t\t\tEviction finished at %lu", evDoneCycle);
                            newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                            // // // info("SHOULDN'T/SHOULD DOWN");
                            tagArray->postinsert(0, &req, victimListHeadId, -1, -1, NONE, -1, false);
                        } else {
                            newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                        }
                        if (evRec->hasRecord()) {
                            Evictions++;
                            writebackRecord.clear();
                            writebackRecord = evRec->popRecord();
                            writebackRecords.push_back(writebackRecord);
                            wbStartCycles.push_back(evBeginCycle);
                            wbEndCycles.push_back(evDoneCycle);
                            lastEvDoneCycle = evDoneCycle;
                            evBeginCycle += accLat;
                        }
                        victimListHeadId = newVictimListHeadId;
                    }
                    // info("\t\tand freed %i segments", size);
                    dataArray->postinsert(-1, &req, 0, victimDataId, victimSegmentId, NULL, false);
                } while (freeSpace < lineSize);
                // // // info("SHOULD UP");
                tagArray->postinsert(req.lineAddr, &req, victimTagId, victimDataId, keptFromEvictions[0], encoding, -1, true);
                // // info("postinsert %i", victimTagId);
                dataArray->postinsert(victimTagId, &req, 1, victimDataId, keptFromEvictions[0], data, updateReplacement);
                assert_msg(getDoneCycle == respCycle, "gdc %ld rc %ld", getDoneCycle, respCycle);
                mse = new (evRec) MissStartEvent(this, accLat, domain);
                // // // info("uCREATE: %p at %u", mse, __LINE__);
                mre = new (evRec) MissResponseEvent(this, mse, domain);
                // // // info("uCREATE: %p at %u", mre, __LINE__);
                mwe = new (evRec) MissWritebackEvent(this, mse, accLat, domain);
                // // // info("uCREATE: %p at %u", mwe, __LINE__);

                mse->setMinStartCycle(req.cycle);
                // // // info("\t\t\tMiss Start Event: %lu, %u", req.cycle, accLat);
                mre->setMinStartCycle(respCycle);
                // // // info("\t\t\tMiss Response Event: %lu", respCycle);
                mwe->setMinStartCycle(MAX(lastEvDoneCycle, tagEvDoneCycle));
                // // // info("\t\t\tMiss writeback event: %lu, %u", MAX(lastEvDoneCycle, tagEvDoneCycle), accLat);

                connect(accessRecord.isValid()? &accessRecord : nullptr, mse, mre, req.cycle + accLat, respCycle);
                if(wbStartCycles.size()) {
                    for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                        DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - respCycle);
                        // // // info("uCREATE: %p at %u", del, __LINE__);
                        del->setMinStartCycle(respCycle);
                        mre->addChild(del, evRec);
                        connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, del, mwe, wbStartCycles[i], wbEndCycles[i]);
                    }
                }
                mre->addChild(mwe, evRec);
                if (tagEvDoneCycle) {
                    connect(tagWritebackRecord.isValid()? &tagWritebackRecord : nullptr, mse, mwe, req.cycle + accLat, tagEvDoneCycle);
                }
            }
            tr.startEvent = mse;
            tr.endEvent = mre;
        } else {
            if (tag_hits) tag_hits->inc();
            zinfo->tagHits++;
            if(approximate)
                hashArray->approximate(data, type);
            uint16_t lineSize = 0;
            BDICompressionEncoding encoding = dataArray->compress(data, &lineSize);
            // info("\tHit Data Size: %u Segments", lineSize/8);
            int32_t dataId = tagArray->readDataId(tagId);
            int32_t segmentId = tagArray->readSegmentPointer(tagId);

            if (req.type == PUTX && !dataArray->isSame(dataId, segmentId, data)) {
                // info("\tWrite Tag Hit, Data different");
                // // info("PUTX Hit Req");
                int32_t targetDataId = -1;
                int32_t targetSegmentId = -1;
                for (uint32_t i = 0; i < numDataLines/dataAssoc; i++) {
                    for (uint32_t j = 0; j < dataAssoc*8; j++) {
                        if (dataArray->readCounter(i, j) && dataArray->isSame(i, j, data)) {
                            targetDataId = i;
                            targetSegmentId = j;
                            break;
                        }
                    }
                }
                if (targetDataId != -1) {
                    // // info("Hash Hit");
                    WD_TH_DS++;
                    // info("\t\tFound matching data at %i.", targetDataId);
                    respCycle += accLat;
                    int32_t newLLHead;
                    bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead);
                    if (evictDataLine) {
                        // info("\t\tDeleting old dataId at %i", dataId);
                        // // info("\t\tAlong with dataId: %i", dataId);
                        // Clear (Evict, Tags already evicted) data line
                        dataArray->postinsert(-1, &req, 0, dataId, segmentId, NULL, false);
                        tagArray->postinsert(0, &req, tagId, -1, -1, NONE, -1, false, false);
                    } else if (newLLHead != -1) {
                        // info("\t\tchanging LL pointer for old dataId at %i and decremented it's counter", dataId);
                        // Change Tag
                        uint32_t victimCounter = dataArray->readCounter(dataId, segmentId);
                        dataArray->changeInPlace(newLLHead, &req, victimCounter-1, dataId, segmentId, NULL, false);
                    } else {
                        // info("\t\tdecremented the counter at dataId %i", dataId);
                        uint32_t victimCounter = dataArray->readCounter(dataId, segmentId);
                        int32_t LLHead = dataArray->readListHead(dataId, segmentId);
                        dataArray->changeInPlace(LLHead, &req, victimCounter-1, dataId, segmentId, NULL, false);
                    }
                    // // info("Data is also similar.");
                    int32_t oldListHead = dataArray->readListHead(targetDataId, targetSegmentId);
                    uint32_t dataCounter = dataArray->readCounter(targetDataId, targetSegmentId);
                    // // // info("SHOULDN'T");
                    tagArray->changeInPlace(req.lineAddr, &req, tagId, targetDataId, targetSegmentId, encoding, oldListHead, true);
                    // // info("postinsert %i", tagId);
                    dataArray->changeInPlace(tagId, &req, dataCounter+1, targetDataId, targetSegmentId, NULL, updateReplacement);
                    uint64_t getDoneCycle = respCycle;
                    respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                    if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                    tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                    HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                    // // // info("uCREATE: %p at %u", ev, __LINE__);
                    // // // info("\t\t\tHit Event: %lu, %lu", req.cycle, respCycle - req.cycle);
                    ev->setMinStartCycle(req.cycle);
                    tr.startEvent = tr.endEvent = ev;
                } else {
                    // info("\t\tCouldn't find a matching hash.");
                    if (dataArray->readCounter(dataId, segmentId) == 1) {
                        WD_TH_DD_1++;
                        // Data only exists once, just update.
                        // // info("PUTX only once.");
                        int32_t newLLHead;
                        bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead);
                        if (evictDataLine) {
                            // info("\t\tDeleting old dataId at %i", dataId);
                            // // info("\t\tAlong with dataId: %i", dataId);
                            // Clear (Evict, Tags already evicted) data line
                            dataArray->postinsert(-1, &req, 0, dataId, segmentId, NULL, false);
                            tagArray->postinsert(0, &req, tagId, -1, -1, NONE, -1, false, false);
                        } else if (newLLHead != -1) {
                            // info("\t\tchanging LL pointer for old dataId at %i and decremented it's counter", dataId);
                            // Change Tag
                            uint32_t victimCounter = dataArray->readCounter(dataId, segmentId);
                            dataArray->changeInPlace(newLLHead, &req, victimCounter-1, dataId, segmentId, NULL, false);
                        } else {
                            // info("\t\tdecremented the counter at dataId %i", dataId);
                            uint32_t victimCounter = dataArray->readCounter(dataId, segmentId);
                            int32_t LLHead = dataArray->readListHead(dataId, segmentId);
                            dataArray->changeInPlace(LLHead, &req, victimCounter-1, dataId, segmentId, NULL, false);
                        }
                        int32_t victimDataId = dataArray->preinsert(lineSize);
                        // Now we need to know the available space in this set
                        uint16_t freeSpace = 0;
                        g_vector<uint32_t> keptFromEvictions;
                        // info("\t\tOnly had one tag. picked victim dataId: %i", victimDataId);
                        evictCycle += accLat;
                        uint64_t lastEvDoneCycle = evictCycle;
                        uint64_t evBeginCycle = evictCycle;
                        do {
                            uint16_t occupiedSpace = 0;
                            for (uint32_t i = 0; i < dataArray->getAssoc()*zinfo->lineSize/8; i++)
                                if (dataArray->readListHead(victimDataId, i) != -1)
                                    occupiedSpace += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, i)), zinfo->lineSize);
                            freeSpace = dataArray->getAssoc()*zinfo->lineSize - occupiedSpace;
                            // info("\t\tFree Space %i segments", freeSpace/8);
                            int32_t victimListHeadId, newVictimListHeadId;
                            int32_t victimSegmentId = dataArray->preinsert(victimDataId, &victimListHeadId, keptFromEvictions);
                            // uint32_t size = 0;
                            if (dataArray->readListHead(victimDataId, victimSegmentId) != -1) {
                                freeSpace += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, victimSegmentId)), zinfo->lineSize);
                                // size = BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, victimSegmentId)), zinfo->lineSize)/8;
                            }
                            // info("\t\tEvicting dataline %i,%i", victimDataId, victimSegmentId);
                            keptFromEvictions.push_back(victimSegmentId);
                            uint64_t evDoneCycle = evBeginCycle;
                            TimingRecord writebackRecord;
                            lastEvDoneCycle = evBeginCycle;
                            if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                            while (victimListHeadId != -1) {
                                if (victimListHeadId != tagId) {
                                    // info("\t\tEvicting TagId: %i", victimListHeadId);
                                    Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                                    // // info("\t\tEvicting tagId: %i, %lu", victimListHeadId, wbLineAddr);
                                    evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                                    // // // info("\t\t\tEviction finished at %lu", evDoneCycle);
                                    newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                                    // // // info("SHOULDN'T/SHOULD DOWN");
                                    tagArray->postinsert(0, &req, victimListHeadId, -1, -1, NONE, -1, false);
                                } else {
                                    newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                                }
                                if (evRec->hasRecord()) {
                                    Evictions++;
                                    writebackRecord.clear();
                                    writebackRecord = evRec->popRecord();
                                    writebackRecords.push_back(writebackRecord);
                                    wbStartCycles.push_back(evBeginCycle);
                                    wbEndCycles.push_back(evDoneCycle);
                                    lastEvDoneCycle = evDoneCycle;
                                    evBeginCycle += accLat;
                                }
                                victimListHeadId = newVictimListHeadId;
                            }
                            // info("\t\tand freed %i segments", size);
                            dataArray->postinsert(-1, &req, 0, victimDataId, victimSegmentId, NULL, false);
                        } while (freeSpace < lineSize);
                        respCycle = lastEvDoneCycle;
                        // // // info("SHOULD CHANGE");
                        tagArray->postinsert(req.lineAddr, &req, tagId, victimDataId, keptFromEvictions[0], encoding, -1, updateReplacement, false);
                        dataArray->postinsert(tagId, &req, 1, victimDataId, keptFromEvictions[0], data, updateReplacement);
                        uint64_t getDoneCycle = respCycle;
                        respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                        if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                        tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                        HitEvent* he = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                        // // // info("uCREATE: %p at %u", he, __LINE__);
                        idbHitWritebackEvent* hwe = new (evRec) idbHitWritebackEvent(this, he, respCycle - req.cycle, domain);
                        // // // info("uCREATE: %p at %u", hwe, __LINE__);

                        he->setMinStartCycle(req.cycle);
                        hwe->setMinStartCycle(lastEvDoneCycle);
                        // // // info("\t\t\tHit Event: %lu, %lu", req.cycle, respCycle - req.cycle);
                        // // // info("\t\t\tHit writeback Event: %lu, %lu", lastEvDoneCycle, respCycle - req.cycle);

                        if(wbStartCycles.size()) {
                            for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                                DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - (req.cycle + accLat));
                                // // // info("uCREATE: %p at %u", del, __LINE__);
                                del->setMinStartCycle(req.cycle + accLat);
                                he->addChild(del, evRec);
                                connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, del, hwe, wbStartCycles[i], wbEndCycles[i]);
                            }
                        }
                        he->addChild(hwe, evRec);
                        tr.startEvent = tr.endEvent = he;
                    } else {
                        WD_TH_DD_M++;
                        // Data exists more than once, evict from LL.
                        // // info("PUTX more than once");
                        int32_t newLLHead;
                        bool evictDataLine = tagArray->evictAssociatedData(tagId, &newLLHead);
                        if (evictDataLine) {
                            panic("Shouldn't happen %i, %i, %i", tagId, dataId, segmentId);
                        } else if (newLLHead != -1) {
                            // info("\t\tchanging LL pointer for old dataId at %i and decremented it's counter", dataId);
                            // Change Tag
                            uint32_t victimCounter = dataArray->readCounter(dataId, segmentId);
                            dataArray->changeInPlace(newLLHead, &req, victimCounter-1, dataId, segmentId, NULL, false);
                        } else {
                            // info("\t\tdecremented the counter at dataId %i", dataId);
                            uint32_t victimCounter = dataArray->readCounter(dataId, segmentId);
                            int32_t LLHead = dataArray->readListHead(dataId, segmentId);
                            dataArray->changeInPlace(LLHead, &req, victimCounter-1, dataId, segmentId, NULL, false);
                        }
                        int32_t victimDataId = dataArray->preinsert(lineSize);

                        // Now we need to know the available space in this set
                        uint16_t freeSpace = 0;
                        g_vector<uint32_t> keptFromEvictions;
                        evictCycle += accLat;
                        uint64_t lastEvDoneCycle = evictCycle;
                        uint64_t evBeginCycle = evictCycle;
                        do {
                            uint16_t occupiedSpace = 0;
                            for (uint32_t i = 0; i < dataArray->getAssoc()*zinfo->lineSize/8; i++)
                                if (dataArray->readListHead(victimDataId, i) != -1)
                                    occupiedSpace += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, i)), zinfo->lineSize);
                            freeSpace = dataArray->getAssoc()*zinfo->lineSize - occupiedSpace;
                            // info("\t\tFree Space %i segments", freeSpace/8);
                            int32_t victimListHeadId, newVictimListHeadId;
                            int32_t victimSegmentId = dataArray->preinsert(victimDataId, &victimListHeadId, keptFromEvictions);
                            // uint32_t size = 0;
                            if (dataArray->readListHead(victimDataId, victimSegmentId) != -1) {
                                freeSpace += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, victimSegmentId)), zinfo->lineSize);
                                // size = BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(victimDataId, victimSegmentId)), zinfo->lineSize)/8;
                            }
                            // info("\t\tEvicting dataline %i,%i", victimDataId, victimSegmentId);
                            keptFromEvictions.push_back(victimSegmentId);
                            uint64_t evDoneCycle = evBeginCycle;
                            TimingRecord writebackRecord;
                            lastEvDoneCycle = evBeginCycle;
                            if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                            while (victimListHeadId != -1) {
                                if (victimListHeadId != tagId) {
                                    // info("\t\tEvicting TagId: %i", victimListHeadId);
                                    Address wbLineAddr = tagArray->readAddress(victimListHeadId);
                                    // // info("\t\tEvicting tagId: %i, %lu", victimListHeadId, wbLineAddr);
                                    evDoneCycle = cc->processEviction(req, wbLineAddr, victimListHeadId, evBeginCycle);
                                    // // // info("\t\t\tEviction finished at %lu", evDoneCycle);
                                    newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                                    // // // info("SHOULDN'T/SHOULD DOWN");
                                    tagArray->postinsert(0, &req, victimListHeadId, -1, -1, NONE, -1, false);
                                } else {
                                    newVictimListHeadId = tagArray->readNextLL(victimListHeadId);
                                }
                                if (evRec->hasRecord()) {
                                    Evictions++;
                                    writebackRecord.clear();
                                    writebackRecord = evRec->popRecord();
                                    writebackRecords.push_back(writebackRecord);
                                    wbStartCycles.push_back(evBeginCycle);
                                    wbEndCycles.push_back(evDoneCycle);
                                    lastEvDoneCycle = evDoneCycle;
                                    evBeginCycle += accLat;
                                }
                                victimListHeadId = newVictimListHeadId;
                            }
                            // info("\t\tand freed %i segments", size);
                            dataArray->postinsert(-1, &req, 0, victimDataId, victimSegmentId, NULL, false);
                        } while (freeSpace < lineSize);
                        respCycle = lastEvDoneCycle;
                        // // // info("SHOULD UP");
                        tagArray->postinsert(req.lineAddr, &req, tagId, victimDataId, keptFromEvictions[0], encoding, -1, updateReplacement, false);
                        // // info("postinsert %i", tagId);
                        dataArray->postinsert(tagId, &req, 1, victimDataId, keptFromEvictions[0], data, updateReplacement);
                        uint64_t getDoneCycle = respCycle;
                        respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                        if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                        tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};

                        HitEvent* he = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                        // // // info("uCREATE: %p at %u", he, __LINE__);
                        idbHitWritebackEvent* hwe = new (evRec) idbHitWritebackEvent(this, he, respCycle - req.cycle, domain);
                        // // // info("uCREATE: %p at %u", hwe, __LINE__);

                        he->setMinStartCycle(req.cycle);
                        hwe->setMinStartCycle(lastEvDoneCycle);
                        // // // info("\t\t\tHit Event: %lu, %lu", req.cycle, respCycle - req.cycle);
                        // // // info("\t\t\tHit writeback Event: %lu, %lu", lastEvDoneCycle, respCycle - req.cycle);

                        if(wbStartCycles.size()) {
                            for(uint32_t i = 0; i < wbStartCycles.size(); i++) {
                                DelayEvent* del = new (evRec) DelayEvent(wbStartCycles[i] - (req.cycle + accLat));
                                // // // info("uCREATE: %p at %u", del, __LINE__);
                                del->setMinStartCycle(req.cycle + accLat);
                                he->addChild(del, evRec);
                                connect(writebackRecords[i].isValid()? &writebackRecords[i] : nullptr, del, hwe, wbStartCycles[i], wbEndCycles[i]);
                            }
                        }
                        he->addChild(hwe, evRec);
                        tr.startEvent = tr.endEvent = he;
                    }
                }
            } else {
                WSR_TH++;
                // info("\tHit Req");
                dataArray->lookup(tagArray->readDataId(tagId), tagArray->readSegmentPointer(tagId), &req, updateReplacement);
                respCycle += accLat;
                uint64_t getDoneCycle = respCycle;
                respCycle = cc->processAccess(req, tagId, respCycle, &getDoneCycle);
                if (evRec->hasRecord()) accessRecord = evRec->popRecord();
                tr = {req.lineAddr << lineBits, req.cycle, respCycle, req.type, nullptr, nullptr};
                HitEvent* ev = new (evRec) HitEvent(this, respCycle - req.cycle, domain);
                // // // info("uCREATE: %p at %u", ev, __LINE__);
                // // // // info("\t\t\tHit Event: %lu, %lu", req.cycle, respCycle - req.cycle);
                ev->setMinStartCycle(req.cycle);
                tr.startEvent = tr.endEvent = ev;
            }
        }
        gm_free(data);
        evRec->pushRecord(tr);

        // tagArray->print();
        // dataArray->print();
        // hashArray->print();
    }
    cc->endAccess(req);

    // uint32_t dataValidSegments = 0;
    // for (uint32_t i = 0; i < numDataLines/dataAssoc; i++)
    // {
    //     uint32_t singleSetCount = 0;
    //     for (uint32_t j = 0; j < dataAssoc*8; j++)
    //     {
    //         if (dataArray->readListHead(i, j) != -1) {
    //             dataValidSegments += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(i, j)), zinfo->lineSize)/8;
    //             singleSetCount += BDICompressionToSize(tagArray->readCompressionEncoding(dataArray->readListHead(i, j)), zinfo->lineSize)/8;
    //         }
    //         assert(singleSetCount <= dataAssoc*8);
    //     }
    // }

    // uint32_t count = 0;
    // for (int32_t i = 0; i < (signed)(numDataLines/dataAssoc); i++) {
    //     for (int32_t j = 0; j < (signed)dataAssoc*8; j++) {
    //         if (dataArray->readListHead(i, j) == -1)
    //             continue;
    //         count += dataArray->readCounter(i, j);
    //         int32_t tagId = dataArray->readListHead(i, j);
    //         assert(tagArray->readDataId(tagId) == i && tagArray->readSegmentPointer(tagId) == j);
    //     }
    // }
    // assert(count == tagArray->getValidLines());

    // info("Valid Tags: %u", tagArray->getValidLines());
    // info("Valid Segments: %u", tagArray->getDataValidSegments());
    // assert(tagArray->getValidLines() == tagArray->countValidLines());
    // assert(tagArray->getDataValidSegments() == dataValidSegments);
    assert(tagArray->getValidLines() >= tagArray->getDataValidSegments()/8);
    assert(tagArray->getValidLines() <= numTagLines);
    assert(tagArray->getDataValidSegments() <= numDataLines*8);

    double sample = ((double)tagArray->getDataValidSegments()/8)/(double)tagArray->getValidLines();
    crStats->add(sample,1);

    if (req.type != PUTS) {
        sample = Evictions;
        evStats->add(sample,1);
    }

    sample = ((double)tagArray->getDataValidSegments()/8)/numDataLines;
    dutStats->add(sample, 1);

    sample = (double)tagArray->getValidLines()/numTagLines;
    tutStats->add(sample, 1);

    uint32_t compressedLineCount = 0;
    for (uint32_t i = 0; i < numDataLines/dataAssoc; i++) {
        for (uint32_t j = 0; j < dataAssoc*8; j++) {
            if(dataArray->readListHead(i, j) != -1) {
                compressedLineCount++;
            }
        }
    }

    sample = (double)tagArray->getValidLines()/compressedLineCount;
    dupStats->add(sample, 1);

    sample = (double)tagArray->getDataValidSegments()/compressedLineCount;
    bdiStats->add(sample, 1);

    assert_msg(respCycle >= req.cycle, "[%s] resp < req? 0x%lx type %s childState %s, respCycle %ld reqCycle %ld",
            name.c_str(), req.lineAddr, AccessTypeName(req.type), MESIStateName(*req.state), respCycle, req.cycle);
    return respCycle;
}

void ApproximateIdealDedupBDICache::simulateHitWriteback(idbHitWritebackEvent* ev, uint64_t cycle, HitEvent* he) {
    uint64_t lookupCycle = tryLowPrioAccess(cycle);
    if (lookupCycle) { //success, release MSHR
        if (!pendingQueue.empty()) {
            //// // info("XXX %ld elems in pending queue", pendingQueue.size());
            for (TimingEvent* qev : pendingQueue) {
                qev->requeue(cycle+1);
            }
            pendingQueue.clear();
        }
        ev->done(cycle);
    } else {
        ev->requeue(cycle+1);
    }
}

void ApproximateIdealDedupBDICache::dumpStats() {
    info("TM_DS: %lu", TM_DS);
    info("TM_DD: %lu", TM_DD);
    info("WD_TH_DS: %lu", WD_TH_DS);
    info("WD_TH_DD_1: %lu", WD_TH_DD_1);
    info("WD_TH_DD_M: %lu", WD_TH_DD_M);
    info("WSR_TH: %lu", WSR_TH);
    dupStats->dump();
    bdiStats->dump();
}