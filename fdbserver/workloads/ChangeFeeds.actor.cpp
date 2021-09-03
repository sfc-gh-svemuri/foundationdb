/*
 * ChangeFeeds.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fdbclient/FDBOptions.g.h"
#include "fdbclient/NativeAPI.actor.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/workloads/BulkSetup.actor.h"
#include "flow/Arena.h"
#include "flow/IRandom.h"
#include "flow/Trace.h"
#include "flow/actorcompiler.h" // This must be the last #include.
#include "flow/serialize.h"
#include <cstring>

ACTOR Future<std::pair<Standalone<VectorRef<KeyValueRef>>, Version>> readDatabase(Database cx) {
	state Transaction tr(cx);
	loop {
		state Standalone<VectorRef<KeyValueRef>> output;
		state Version readVersion;
		try {
			Version ver = wait(tr.getReadVersion());
			readVersion = ver;

			state PromiseStream<Standalone<RangeResultRef>> results;
			state Future<Void> stream = tr.getRangeStream(results, normalKeys, 1e6);

			loop {
				Standalone<RangeResultRef> res = waitNext(results.getFuture());
				output.append(output.arena(), res.begin(), res.size());
			}
		} catch (Error& e) {
			if (e.code() == error_code_end_of_stream) {
				return std::make_pair(output, readVersion);
			}
			wait(tr.onError(e));
		}
	}
}

ACTOR Future<Standalone<VectorRef<MutationsAndVersionRef>>> readMutations(Database cx,
                                                                          Key rangeID,
                                                                          Version begin,
                                                                          Version end) {
	state Standalone<VectorRef<MutationsAndVersionRef>> output;

	loop {
		try {
			state PromiseStream<Standalone<VectorRef<MutationsAndVersionRef>>> results;
			state Future<Void> stream = cx->getChangeFeedStream(results, rangeID, begin, end, normalKeys);
			loop {
				Standalone<VectorRef<MutationsAndVersionRef>> res = waitNext(results.getFuture());
				output.arena().dependsOn(res.arena());
				output.append(output.arena(), res.begin(), res.size());
				begin = res.back().version + 1;
			}
		} catch (Error& e) {
			if (e.code() == error_code_end_of_stream) {
				return output;
			}
			throw;
		}
	}
}

Standalone<VectorRef<KeyValueRef>> advanceData(Standalone<VectorRef<KeyValueRef>> source,
                                               Standalone<VectorRef<MutationsAndVersionRef>> mutations) {
	std::map<KeyRef, ValueRef> data;
	for (auto& kv : source) {
		data[kv.key] = kv.value;
	}
	for (auto& it : mutations) {
		for (auto& m : it.mutations) {
			if (m.type == MutationRef::SetValue) {
				data[m.param1] = m.param2;
			} else {
				ASSERT(m.type == MutationRef::ClearRange);
				data.erase(data.lower_bound(m.param1), data.lower_bound(m.param2));
			}
		}
	}
	Standalone<VectorRef<KeyValueRef>> output;
	output.arena().dependsOn(source.arena());
	output.arena().dependsOn(mutations.arena());
	for (auto& kv : data) {
		output.push_back(output.arena(), KeyValueRef(kv.first, kv.second));
	}
	return output;
}

bool compareData(Standalone<VectorRef<KeyValueRef>> source, Standalone<VectorRef<KeyValueRef>> dest) {
	if (source.size() != dest.size()) {
		TraceEvent(SevError, "ChangeFeedSizeMismatch").detail("SrcSize", source.size()).detail("DestSize", dest.size());
		return false;
	}
	for (int i = 0; i < source.size(); i++) {
		if (source[i] != dest[i]) {
			TraceEvent("ChangeFeedMutationMismatch")
			    .detail("Index", i)
			    .detail("SrcKey", source[i].key)
			    .detail("DestKey", dest[i].key)
			    .detail("SrcValue", source[i].value)
			    .detail("DestValue", dest[i].value);
			return false;
		}
	}
	return true;
}

struct ChangeFeedsWorkload : TestWorkload {
	double testDuration;
	Future<Void> client;

	ChangeFeedsWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		testDuration = getOption(options, "testDuration"_sr, 10.0);
	}

	std::string description() const override { return "ChangeFeedsWorkload"; }
	Future<Void> setup(Database const& cx) override { return Void(); }
	Future<Void> start(Database const& cx) override {
		client = changeFeedClient(cx->clone(), this);
		return delay(testDuration);
	}
	Future<bool> check(Database const& cx) override {
		client = Future<Void>();
		return true;
	}
	void getMetrics(vector<PerfMetric>& m) override {}

	ACTOR Future<Void> changeFeedClient(Database cx, ChangeFeedsWorkload* self) {
		// Enable change feeds for a key range
		state Key rangeID = StringRef(deterministicRandom()->randomUniqueID().toString());
		state Transaction tr(cx);
		loop {
			try {
				wait(tr.registerChangeFeed(rangeID, normalKeys));
				wait(tr.commit());
				break;
			} catch (Error& e) {
				wait(tr.onError(e));
			}
		}

		loop {
			wait(delay(deterministicRandom()->random01()));

			state std::pair<Standalone<VectorRef<KeyValueRef>>, Version> firstResults = wait(readDatabase(cx));

			wait(delay(10 * deterministicRandom()->random01()));

			state std::pair<Standalone<VectorRef<KeyValueRef>>, Version> secondResults = wait(readDatabase(cx));
			state Standalone<VectorRef<MutationsAndVersionRef>> mutations =
			    wait(readMutations(cx, rangeID, firstResults.second, secondResults.second));

			Standalone<VectorRef<KeyValueRef>> advancedResults = advanceData(firstResults.first, mutations);

			if (!compareData(secondResults.first, advancedResults)) {
				TraceEvent(SevError, "ChangeFeedMismatch")
				    .detail("FirstVersion", firstResults.second)
				    .detail("SecondVersion", secondResults.second);
				for (int i = 0; i < secondResults.first.size(); i++) {
					TraceEvent("ChangeFeedBase")
					    .detail("Index", i)
					    .detail("K", secondResults.first[i].key)
					    .detail("V", secondResults.first[i].value);
				}
				for (int i = 0; i < advancedResults.size(); i++) {
					TraceEvent("ChangeFeedAdvanced")
					    .detail("Index", i)
					    .detail("K", advancedResults[i].key)
					    .detail("V", advancedResults[i].value);
				}
			}

			wait(cx->popChangeFeedMutations(rangeID, secondResults.second));
		}
	}
};

WorkloadFactory<ChangeFeedsWorkload> ChangeFeedsWorkloadFactory("ChangeFeeds");
