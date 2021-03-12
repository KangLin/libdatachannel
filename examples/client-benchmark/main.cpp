/*
 * libdatachannel client-benchmark example
 * Copyright (c) 2019-2020 Paul-Louis Ageneau
 * Copyright (c) 2019-2021 Murat Dogan
 * Copyright (c) 2020 Will Munn
 * Copyright (c) 2020 Nico Chatzi
 * Copyright (c) 2020 Lara Mackey
 * Copyright (c) 2020 Erik Cota-Robles
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; If not, see <http://www.gnu.org/licenses/>.
 */

#include "rtc/rtc.hpp"

#include "parse_cl.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <future>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <thread>
#include <unordered_map>

using namespace rtc;
using namespace std;
using namespace std::chrono_literals;

using json = nlohmann::json;

template <class T> weak_ptr<T> make_weak_ptr(shared_ptr<T> ptr) { return ptr; }

unordered_map<string, shared_ptr<PeerConnection>> peerConnectionMap;
unordered_map<string, shared_ptr<DataChannel>> dataChannelMap;

string localId;

shared_ptr<PeerConnection> createPeerConnection(const Configuration &config,
                                                weak_ptr<WebSocket> wws, string id);
string randomId(size_t length);

// Benchmark
const size_t messageSize = 65535;
binary messageData(messageSize);
atomic<size_t> receivedSize = 0, sentSize = 0;

int main(int argc, char **argv) try {
	Cmdline params(argc, argv);

	rtc::InitLogger(LogLevel::Info);

	// Benchmark - construct message to send
	fill(messageData.begin(), messageData.end(), std::byte(0xFF));

	Configuration config;
	string stunServer = "";
	if (params.noStun()) {
		cout << "No STUN server is configured. Only local hosts and public IP addresses supported."
		     << endl;
	} else {
		if (params.stunServer().substr(0, 5).compare("stun:") != 0) {
			stunServer = "stun:";
		}
		stunServer += params.stunServer() + ":" + to_string(params.stunPort());
		cout << "Stun server is " << stunServer << endl;
		config.iceServers.emplace_back(stunServer);
	}

	localId = randomId(4);
	cout << "The local ID is: " << localId << endl;

	auto ws = make_shared<WebSocket>();

	std::promise<void> wsPromise;
	auto wsFuture = wsPromise.get_future();

	ws->onOpen([&wsPromise]() {
		cout << "WebSocket connected, signaling ready" << endl;
		wsPromise.set_value();
	});

	ws->onError([&wsPromise](string s) {
		cout << "WebSocket error" << endl;
		wsPromise.set_exception(std::make_exception_ptr(std::runtime_error(s)));
	});

	ws->onClosed([]() { cout << "WebSocket closed" << endl; });

	ws->onMessage([&](variant<binary, string> data) {
		if (!holds_alternative<string>(data))
			return;

		json message = json::parse(get<string>(data));

		auto it = message.find("id");
		if (it == message.end())
			return;
		string id = it->get<string>();

		it = message.find("type");
		if (it == message.end())
			return;
		string type = it->get<string>();

		shared_ptr<PeerConnection> pc;
		if (auto jt = peerConnectionMap.find(id); jt != peerConnectionMap.end()) {
			pc = jt->second;
		} else if (type == "offer") {
			cout << "Answering to " + id << endl;
			pc = createPeerConnection(config, ws, id);
		} else {
			return;
		}

		if (type == "offer" || type == "answer") {
			auto sdp = message["description"].get<string>();
			pc->setRemoteDescription(Description(sdp, type));
		} else if (type == "candidate") {
			auto sdp = message["candidate"].get<string>();
			auto mid = message["mid"].get<string>();
			pc->addRemoteCandidate(Candidate(sdp, mid));
		}
	});

	string wsPrefix = "";
	if (params.webSocketServer().substr(0, 5).compare("ws://") != 0) {
		wsPrefix = "ws://";
	}
	const string url = wsPrefix + params.webSocketServer() + ":" +
	                   to_string(params.webSocketPort()) + "/" + localId;
	cout << "Url is " << url << endl;
	ws->open(url);

	cout << "Waiting for signaling to be connected..." << endl;
	wsFuture.get();

	string id;
	cout << "Enter a remote ID to send an offer:" << endl;
	cin >> id;
	cin.ignore();
	if (id.empty()) {
		// Nothing to do
		return 0;
	}
	if (id == localId) {
		cout << "Invalid remote ID (This is my local ID). Exiting..." << endl;
		return 0;
	}

	cout << "Offering to " + id << endl;
	auto pc = createPeerConnection(config, ws, id);

	// We are the offerer, so create a data channel to initiate the process
	const string label = "benchmark";
	cout << "Creating DataChannel with label \"" << label << "\"" << endl;
	auto dc = pc->createDataChannel(label);

	dc->onOpen([id, wdc = make_weak_ptr(dc)]() {
		cout << "DataChannel from " << id << " open" << endl;
		if (auto dcLocked = wdc.lock()) {
			cout << "Starting benchmark test. Sending data..." << endl;
			try {
				while (dcLocked->bufferedAmount() == 0) {
					dcLocked->send(messageData);
					sentSize += messageData.size();
				}
			} catch (const std::exception &e) {
				std::cout << "Send failed: " << e.what() << std::endl;
			}
		}
	});

	dc->onBufferedAmountLow([wdc = make_weak_ptr(dc)]() {
		auto dcLocked = wdc.lock();
		if (!dcLocked)
			return;

		// Continue sending
		try {
			while (dcLocked->isOpen() && dcLocked->bufferedAmount() == 0) {
				dcLocked->send(messageData);
				sentSize += messageData.size();
			}
		} catch (const std::exception &e) {
			std::cout << "Send failed: " << e.what() << std::endl;
		}
	});

	dc->onClosed([id]() { cout << "DataChannel from " << id << " closed" << endl; });

	dc->onMessage([id, wdc = make_weak_ptr(dc)](variant<binary, string> data) {
		if (holds_alternative<binary>(data))
			receivedSize += get<binary>(data).size();
	});

	dataChannelMap.emplace(id, dc);

	const int duration = params.durationInSec() > 0 ? params.durationInSec() : INT32_MAX;
	cout << "Becnhmark will run for " << duration << " seconds" << endl;

	int printStatCounter = 0;

	for (int i = 1; i <= duration; ++i) {
		this_thread::sleep_for(1000ms);
		cout << "#" << i << " Received: " << receivedSize.load() / 1024 << " KB/s"
		     << "   Sent: " << sentSize.load() / 1024 << " KB/s"
		     << "   BufferSize: " << dc->bufferedAmount() << endl;
		receivedSize = sentSize = 0;
		printStatCounter++;

		if (printStatCounter % 5 == 0) {
			cout << "Stats# "
			     << "Received Total: " << pc->bytesReceived() / (1024 * 1024) << " MB"
			     << "   Sent Total: " << pc->bytesSent() / (1024 * 1024) << " MB"
			     << "   RTT: " << pc->rtt().value_or(0ms).count() << " ms" << endl;
			// cout << "Selected Candidates# " << endl;
			cout << endl;
		}
	}

	cout << "Cleaning up..." << endl;

	dataChannelMap.clear();
	peerConnectionMap.clear();
	return 0;

} catch (const std::exception &e) {
	std::cout << "Error: " << e.what() << std::endl;
	dataChannelMap.clear();
	peerConnectionMap.clear();
	return -1;
}

// Create and setup a PeerConnection
shared_ptr<PeerConnection> createPeerConnection(const Configuration &config,
                                                weak_ptr<WebSocket> wws, string id) {
	auto pc = make_shared<PeerConnection>(config);

	pc->onStateChange([](PeerConnection::State state) { cout << "State: " << state << endl; });

	pc->onGatheringStateChange(
	    [](PeerConnection::GatheringState state) { cout << "Gathering State: " << state << endl; });

	pc->onLocalDescription([wws, id](Description description) {
		json message = {
		    {"id", id}, {"type", description.typeString()}, {"description", string(description)}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});

	pc->onLocalCandidate([wws, id](Candidate candidate) {
		json message = {{"id", id},
		                {"type", "candidate"},
		                {"candidate", string(candidate)},
		                {"mid", candidate.mid()}};

		if (auto ws = wws.lock())
			ws->send(message.dump());
	});

	pc->onDataChannel([id](shared_ptr<DataChannel> dc) {
		cout << "DataChannel from " << id << " received with label \"" << dc->label() << "\""
		     << endl;

		cout << "Starting benchmark test. Sending data..." << endl;
		cout << "###########################################" << endl;
		cout << "### Check other peer's screen for stats ###" << endl;
		cout << "###########################################" << endl;
		try {
			while (dc->bufferedAmount() == 0) {
				dc->send(messageData);
				sentSize += messageData.size();
			}
		} catch (const std::exception &e) {
			std::cout << "Send failed: " << e.what() << std::endl;
		}

		dc->onBufferedAmountLow([wdc = make_weak_ptr(dc)]() {
			auto dcLocked = wdc.lock();
			if (!dcLocked)
				return;

			// Continue sending
			try {
				while (dcLocked->isOpen() && dcLocked->bufferedAmount() == 0) {
					dcLocked->send(messageData);
					sentSize += messageData.size();
				}
			} catch (const std::exception &e) {
				std::cout << "Send failed: " << e.what() << std::endl;
			}
		});

		dc->onClosed([id]() { cout << "DataChannel from " << id << " closed" << endl; });

		dc->onMessage([id, wdc = make_weak_ptr(dc)](variant<binary, string> data) {
			if (holds_alternative<binary>(data))
				receivedSize += get<binary>(data).size();
		});

		dataChannelMap.emplace(id, dc);
	});

	peerConnectionMap.emplace(id, pc);
	return pc;
};

// Helper function to generate a random ID
string randomId(size_t length) {
	static const string characters(
	    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz");
	string id(length, '0');
	default_random_engine rng(random_device{}());
	uniform_int_distribution<int> dist(0, int(characters.size() - 1));
	generate(id.begin(), id.end(), [&]() { return characters.at(dist(rng)); });
	return id;
}
