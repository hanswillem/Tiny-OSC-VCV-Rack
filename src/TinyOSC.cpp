#include "plugin.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct TinyOSC;

namespace {

constexpr int DEFAULT_PORT = 10000;
constexpr float MIN_VOLTAGE = -10.f;
constexpr float MAX_VOLTAGE = 10.f;

float clampVoltage(float voltage) {
	if (!std::isfinite(voltage))
		return 0.f;
	return std::min(MAX_VOLTAGE, std::max(MIN_VOLTAGE, voltage));
}

size_t alignOsc(size_t offset) {
	return (offset + 3u) & ~size_t(3u);
}

bool validPort(int port) {
	return port > 0 && port <= 65535;
}

bool readOscString(const uint8_t* data, size_t size, size_t& offset, std::string& text) {
	if (offset >= size)
		return false;

	size_t end = offset;
	while (end < size && data[end] != 0)
		end++;
	if (end >= size)
		return false;

	text.assign(reinterpret_cast<const char*>(data + offset), end - offset);
	offset = alignOsc(end + 1u);
	return offset <= size;
}

bool readUint32BE(const uint8_t* data, size_t size, size_t& offset, uint32_t& value) {
	if (offset + 4u > size)
		return false;

	value = (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16)
		| (uint32_t(data[offset + 2]) << 8) | uint32_t(data[offset + 3]);
	offset += 4u;
	return true;
}

bool readFloat32BE(const uint8_t* data, size_t size, size_t& offset, float& value) {
	uint32_t bits = 0;
	if (!readUint32BE(data, size, offset, bits))
		return false;

	std::memcpy(&value, &bits, sizeof(value));
	return true;
}

bool readInt32BE(const uint8_t* data, size_t size, size_t& offset, float& value) {
	uint32_t bits = 0;
	if (!readUint32BE(data, size, offset, bits))
		return false;

	value = float(static_cast<int32_t>(bits));
	return true;
}

bool addressMatches(const std::string& incoming, const std::string& configured) {
	if (configured.empty())
		return false;
	if (incoming == configured)
		return true;
	if (configured[0] != '/' && incoming == "/" + configured)
		return true;
	return false;
}

bool parseOscMessage(const uint8_t* data, size_t size, const std::string& messageName, float& value) {
	size_t offset = 0;
	std::string address;
	std::string typeTags;

	if (!readOscString(data, size, offset, address))
		return false;
	if (!readOscString(data, size, offset, typeTags))
		return false;
	if (typeTags.empty() || typeTags[0] != ',')
		return false;
	if (!addressMatches(address, messageName))
		return false;

	for (size_t i = 1; i < typeTags.size(); i++) {
		switch (typeTags[i]) {
			case 'f':
				return readFloat32BE(data, size, offset, value);
			case 'i':
				return readInt32BE(data, size, offset, value);
			default:
				return false;
		}
	}

	return false;
}

bool parseOscPacket(const uint8_t* data, size_t size, const std::string& messageName, float& value) {
	if (size >= 8u && std::memcmp(data, "#bundle", 7) == 0) {
		size_t offset = 16u;
		bool parsedAny = false;

		while (offset + 4u <= size) {
			uint32_t elementSize = 0;
			if (!readUint32BE(data, size, offset, elementSize))
				return parsedAny;
			if (elementSize == 0 || offset + elementSize > size)
				return parsedAny;

			parsedAny = parseOscPacket(data + offset, elementSize, messageName, value) || parsedAny;
			offset += elementSize;
		}

		return parsedAny;
	}

	return parseOscMessage(data, size, messageName, value);
}

void registerModuleOnPort(int port, TinyOSC* module);
void unregisterModuleFromPort(int port, TinyOSC* module);

} // namespace

struct TinyOSC : Module {
	enum ParamId {
		SCALE_PARAM,
		OFFSET_PARAM,
		NUM_PARAMS
	};
	enum InputId {
		NUM_INPUTS
	};
	enum OutputId {
		CV_OUTPUT,
		NUM_OUTPUTS
	};
	enum LightId {
		DATA_LIGHT,
		NUM_LIGHTS
	};

	std::atomic<float> value{0.f};
	std::atomic<float> dataPulse{0.f};
	std::atomic<int> port{DEFAULT_PORT};
	bool registered = false;
	std::mutex messageMutex;
	std::string messageName = "/chan1";

	TinyOSC() {
		config(NUM_PARAMS, NUM_INPUTS, NUM_OUTPUTS, NUM_LIGHTS);
		configParam(SCALE_PARAM, MIN_VOLTAGE, MAX_VOLTAGE, MAX_VOLTAGE, "Scale", " V");
		configParam(OFFSET_PARAM, MIN_VOLTAGE, MAX_VOLTAGE, 0.f, "Offset", " V");
		configOutput(CV_OUTPUT, "OSC CV");
		configLight(DATA_LIGHT, "Data received");
		registerReceiver();
	}

	~TinyOSC() override {
		unregisterReceiver();
	}

	std::string getPortText() const {
		return std::to_string(port.load());
	}

	std::string getMessageName() {
		std::lock_guard<std::mutex> lock(messageMutex);
		return messageName;
	}

	void setMessageName(const std::string& text) {
		if (text.empty())
			return;
		std::lock_guard<std::mutex> lock(messageMutex);
		messageName = text;
	}

	void setPortFromText(const std::string& text) {
		if (text.empty())
			return;

		char* end = nullptr;
		long parsed = std::strtol(text.c_str(), &end, 10);
		if (*end != '\0' || !validPort(int(parsed)))
			return;

		int newPort = int(parsed);
		if (newPort == port.load())
			return;

		unregisterReceiver();
		port.store(newPort);
		registerReceiver();
	}

	void registerReceiver() {
		if (registered)
			return;
		registerModuleOnPort(port.load(), this);
		registered = true;
	}

	void unregisterReceiver() {
		if (!registered)
			return;
		unregisterModuleFromPort(port.load(), this);
		registered = false;
	}

	void handlePacket(const uint8_t* data, size_t size) {
		float parsedValue = 0.f;
		std::string currentMessageName = getMessageName();
		if (parseOscPacket(data, size, currentMessageName, parsedValue)) {
			value.store(std::isfinite(parsedValue) ? parsedValue : 0.f);
			dataPulse.store(1.f);
		}
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();
		json_object_set_new(rootJ, "port", json_integer(port.load()));
		json_object_set_new(rootJ, "messageName", json_string(getMessageName().c_str()));
		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {
		unregisterReceiver();

		json_t* portJ = json_object_get(rootJ, "port");
		if (portJ) {
			int savedPort = int(json_integer_value(portJ));
			if (validPort(savedPort))
				port.store(savedPort);
		}

		json_t* messageNameJ = json_object_get(rootJ, "messageName");
		if (messageNameJ) {
			const char* savedMessageName = json_string_value(messageNameJ);
			if (savedMessageName && savedMessageName[0] != '\0')
				setMessageName(savedMessageName);
		}

		registerReceiver();
	}

	void process(const ProcessArgs& args) override {
		float voltage = value.load() * params[SCALE_PARAM].getValue() + params[OFFSET_PARAM].getValue();
		outputs[CV_OUTPUT].setVoltage(clampVoltage(voltage));

		float pulse = dataPulse.load();
		if (pulse > 0.f) {
			pulse = std::max(0.f, pulse - args.sampleTime * 8.f);
			dataPulse.store(pulse);
		}
		lights[DATA_LIGHT].setBrightness(pulse);
	}
};

namespace {

class PortReceiver {
public:
	explicit PortReceiver(int port) : port(port) {}

	void add(TinyOSC* module) {
		std::lock_guard<std::mutex> lock(subscribersMutex);
		if (std::find(subscribers.begin(), subscribers.end(), module) == subscribers.end())
			subscribers.push_back(module);
	}

	bool remove(TinyOSC* module) {
		std::lock_guard<std::mutex> lock(subscribersMutex);
		subscribers.erase(std::remove(subscribers.begin(), subscribers.end(), module), subscribers.end());
		return subscribers.empty();
	}

	void start() {
		running.store(true);
		thread = std::thread([this]() {
			run();
		});
	}

	void stop() {
		running.store(false);
		if (thread.joinable())
			thread.join();
	}

private:
	int port;
	std::atomic<bool> running{false};
	std::thread thread;
	std::mutex subscribersMutex;
	std::vector<TinyOSC*> subscribers;

	void run() {
		int sock = socket(AF_INET, SOCK_DGRAM, 0);
		if (sock < 0)
			return;

		int reuse = 1;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

		timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 100000;
		setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

		sockaddr_in addr;
		std::memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons(port);

		if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
			close(sock);
			return;
		}

		std::array<uint8_t, 1536> buffer;
		while (running.load()) {
			ssize_t received = recvfrom(sock, buffer.data(), buffer.size(), 0, nullptr, nullptr);
			if (received < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					continue;
				break;
			}

			std::lock_guard<std::mutex> lock(subscribersMutex);
			for (TinyOSC* subscriber : subscribers)
				subscriber->handlePacket(buffer.data(), size_t(received));
		}

		close(sock);
	}
};

std::mutex receiversMutex;
std::map<int, std::shared_ptr<PortReceiver>> receivers;

void registerModuleOnPort(int port, TinyOSC* module) {
	std::shared_ptr<PortReceiver> receiver;
	{
		std::lock_guard<std::mutex> lock(receiversMutex);
		auto it = receivers.find(port);
		if (it == receivers.end()) {
			receiver = std::make_shared<PortReceiver>(port);
			receivers[port] = receiver;
			receiver->start();
		}
		else {
			receiver = it->second;
		}
	}

	receiver->add(module);
}

void unregisterModuleFromPort(int port, TinyOSC* module) {
	std::shared_ptr<PortReceiver> receiver;
	bool shouldStop = false;

	{
		std::lock_guard<std::mutex> lock(receiversMutex);
		auto it = receivers.find(port);
		if (it == receivers.end())
			return;

		receiver = it->second;
		shouldStop = receiver->remove(module);
		if (shouldStop)
			receivers.erase(it);
	}

	if (shouldStop)
		receiver->stop();
}

} // namespace

struct TinyOSCTextField : LedDisplayTextField {
	TinyOSC* module = nullptr;
	bool editsPort = false;

	void forceSingleLine() {
		std::string singleLine;
		singleLine.reserve(text.size());
		for (char c : text) {
			if (c != '\n' && c != '\r')
				singleLine.push_back(c);
		}
		if (singleLine != text)
			setText(singleLine);
	}

	void commit() {
		if (!module)
			return;

		forceSingleLine();
		if (editsPort)
			module->setPortFromText(text);
		else
			module->setMessageName(text);
	}

	void onChange(const event::Change& e) override {
		LedDisplayTextField::onChange(e);
		forceSingleLine();
		if (!editsPort)
			commit();
	}

	void onDeselect(const event::Deselect& e) override {
		LedDisplayTextField::onDeselect(e);
		commit();
		if (editsPort && module)
			setText(module->getPortText());
	}

	void onSelectKey(const event::SelectKey& e) override {
		if (e.action == GLFW_PRESS && (e.key == GLFW_KEY_ENTER || e.key == GLFW_KEY_KP_ENTER)) {
			commit();
			e.consume(this);
			return;
		}
		LedDisplayTextField::onSelectKey(e);
	}

};

struct TinyOSCWidget : ModuleWidget {
	TinyOSCWidget(TinyOSC* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/TinyOSC.svg")));

		addChild(createLightCentered<MediumLight<GreenLight>>(mm2px(Vec(10.16, 8.0)), module, TinyOSC::DATA_LIGHT));

		TinyOSCTextField* portField = new TinyOSCTextField;
		portField->module = module;
		portField->editsPort = true;
		portField->multiline = false;
		portField->box.pos = mm2px(Vec(0.2, 12.8));
		portField->box.size = mm2px(Vec(19.9, 8.5));
		portField->placeholder = "10000";
		portField->setText(module ? module->getPortText() : "10000");
		addChild(portField);

		TinyOSCTextField* messageField = new TinyOSCTextField;
		messageField->module = module;
		messageField->editsPort = false;
		messageField->multiline = false;
		messageField->box.pos = mm2px(Vec(0.2, 23.8));
		messageField->box.size = mm2px(Vec(19.9, 8.5));
		messageField->placeholder = "/chan1";
		messageField->setText(module ? module->getMessageName() : "/chan1");
		addChild(messageField);

		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(10.16, 63.1)), module, TinyOSC::SCALE_PARAM));
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(10.16, 75.5)), module, TinyOSC::OFFSET_PARAM));

		addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(10.16, 112.0)), module, TinyOSC::CV_OUTPUT));
	}
};

Model* modelTinyOSC = createModel<TinyOSC, TinyOSCWidget>("TinyOSC");
