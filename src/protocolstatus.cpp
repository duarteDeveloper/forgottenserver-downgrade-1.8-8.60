// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"

#include "protocolstatus.h"

#include "configmanager.h"
#include "game.h"
#include "outputmessage.h"
#include "protocollogin.h"
#include "store/store_catalog.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fmt/format.h>

extern Game g_game;

std::unordered_map<uint32_t, int64_t> ProtocolStatus::ipConnectMap;
std::mutex ProtocolStatus::ipConnectMutex;
int64_t ProtocolStatus::lastCleanup = 0;
const uint64_t ProtocolStatus::start = OTSYS_TIME();

enum RequestedInfo_t : uint16_t
{
	REQUEST_BASIC_SERVER_INFO = 1 << 0,
	REQUEST_OWNER_SERVER_INFO = 1 << 1,
	REQUEST_MISC_SERVER_INFO = 1 << 2,
	REQUEST_PLAYERS_INFO = 1 << 3,
	REQUEST_MAP_INFO = 1 << 4,
	REQUEST_EXT_PLAYERS_INFO = 1 << 5,
	REQUEST_PLAYER_STATUS_INFO = 1 << 6,
	REQUEST_SERVER_SOFTWARE_INFO = 1 << 7,
};

namespace {

// ---------------------------------------------------------------------------
// <tibiaext>: the login screen of TibiaExtendedClientLibrary (8.60 client with
// its ddraw.dll) shows today's boosted creature and boss, a store mount and the
// event schedule. The client reads them from this XML before logging in.
// ---------------------------------------------------------------------------

constexpr std::string_view EVENT_SCHEDULE_FILE = "data/XML/eventschedule.xml";
constexpr size_t MAX_SCHEDULE_EVENTS = 12;
constexpr size_t MAX_STATUS_TEXT = 600;

void appendBoosted(pugi::xml_node parent, const char* tag, const LoginBoostedEntry& entry)
{
	if (!entry.isValid()) {
		return;
	}

	pugi::xml_node node = parent.append_child(tag);
	node.append_attribute("name") = entry.name.c_str();
	node.append_attribute("looktype").set_value(entry.outfit.lookType);
	node.append_attribute("lookhead").set_value(entry.outfit.lookHead);
	node.append_attribute("lookbody").set_value(entry.outfit.lookBody);
	node.append_attribute("looklegs").set_value(entry.outfit.lookLegs);
	node.append_attribute("lookfeet").set_value(entry.outfit.lookFeet);
	node.append_attribute("lookaddons").set_value(entry.outfit.lookAddons);
}

// gamestore.xml descriptions carry {icon} tokens and <i> tags for the store window.
// Line breaks stay (at most one empty line), runs of spaces become one.
std::string plainText(std::string_view text)
{
	std::string out;
	out.reserve(text.size());
	char skipUntil = 0;
	for (const char c : text) {
		if (skipUntil) {
			if (c == skipUntil) {
				skipUntil = 0;
			}
			continue;
		}
		if (c == '{') {
			skipUntil = '}';
			continue;
		}
		if (c == '<') {
			skipUntil = '>';
			continue;
		}
		if (c == '\n') {
			while (!out.empty() && out.back() == ' ') {
				out.pop_back();
			}
			if (out.empty() || out.ends_with("\n\n")) {
				continue;
			}
			out.push_back('\n');
			continue;
		}
		const char clean = (c == '\r' || c == '\t') ? ' ' : c;
		if (clean == ' ' && (out.empty() || out.back() == ' ' || out.back() == '\n')) {
			continue;
		}
		out.push_back(clean);
	}
	while (!out.empty() && (out.back() == ' ' || out.back() == '\n')) {
		out.pop_back();
	}
	if (out.size() > MAX_STATUS_TEXT) {
		out.resize(MAX_STATUS_TEXT);
	}
	return out;
}

// A mount flagged as new, otherwise one per day in catalog order
void appendStoreMount(pugi::xml_node parent, int64_t now)
{
	const auto catalog = StoreManager::getInstance().catalogSnapshot();
	if (!catalog) {
		return;
	}

	std::vector<const StoreOffer*> mounts;
	const StoreOffer* featured = nullptr;
	for (const auto& category : catalog->categories()) {
		for (const auto& offer : category.offers) {
			if (offer.type != StoreOfferType::Mount || offer.displayId == 0) {
				continue;
			}
			mounts.push_back(&offer);
			if (!featured && offer.state == StoreHighlightState::New) {
				featured = &offer;
			}
		}
	}
	if (mounts.empty()) {
		return;
	}
	if (!featured) {
		featured = mounts[static_cast<size_t>(now / 86400) % mounts.size()];
	}

	std::string description = plainText(featured->description);
	if (description.empty()) {
		description = fmt::format("The {} is waiting for you in the Store, for {} Tibia Coins.", featured->name,
		                          featured->price);
	}

	pugi::xml_node node = parent.append_child("storemount");
	node.append_attribute("name") = featured->name.c_str();
	node.append_attribute("looktype").set_value(featured->displayId);
	node.append_attribute("price").set_value(featured->price);
	node.append_attribute("description") = description.c_str();
}

// "2026-10-20" or "2026-10-20 18:00", server local time; the end date counts to 23:59:59
bool parseScheduleDate(const char* text, bool endOfDay, int64_t& out)
{
	int year = 0, month = 0, day = 0, hour = 0, minute = 0;
	const int fields = std::sscanf(text, "%d-%d-%d %d:%d", &year, &month, &day, &hour, &minute);
	if (fields < 3 || year < 2000 || month < 1 || month > 12 || day < 1 || day > 31) {
		return false;
	}

	std::tm tm{};
	tm.tm_year = year - 1900;
	tm.tm_mon = month - 1;
	tm.tm_mday = day;
	if (fields >= 5) {
		tm.tm_hour = hour;
		tm.tm_min = minute;
	} else if (endOfDay) {
		tm.tm_hour = 23;
		tm.tm_min = 59;
		tm.tm_sec = 59;
	}
	tm.tm_isdst = -1;
	const std::time_t value = std::mktime(&tm);
	if (value == static_cast<std::time_t>(-1)) {
		return false;
	}
	out = static_cast<int64_t>(value);
	return true;
}

struct ScheduledEvent
{
	std::string name;
	std::string color;
	std::string description;
	int64_t start = 0;
	int64_t end = 0;
};

void appendEventSchedule(pugi::xml_node parent, int64_t now)
{
	pugi::xml_document doc;
	if (!doc.load_file(std::string{EVENT_SCHEDULE_FILE}.c_str())) {
		return;
	}

	std::vector<ScheduledEvent> events;
	for (const auto& node : doc.child("eventschedule").children("event")) {
		ScheduledEvent event;
		event.name = node.attribute("name").as_string();
		if (event.name.empty() || !parseScheduleDate(node.attribute("startdate").as_string(), false, event.start) ||
		    !parseScheduleDate(node.attribute("enddate").as_string(), true, event.end) || event.end < event.start ||
		    event.end < now) {
			continue;
		}
		event.color = node.attribute("color").as_string();
		event.description = plainText(node.attribute("description").as_string());
		events.push_back(std::move(event));
	}
	std::ranges::sort(events, {}, &ScheduledEvent::start);
	if (events.size() > MAX_SCHEDULE_EVENTS) {
		events.resize(MAX_SCHEDULE_EVENTS);
	}

	pugi::xml_node schedule = parent.append_child("events");
	for (const auto& event : events) {
		pugi::xml_node node = schedule.append_child("event");
		node.append_attribute("name") = event.name.c_str();
		node.append_attribute("start") = std::to_string(event.start).c_str();
		node.append_attribute("end") = std::to_string(event.end).c_str();
		node.append_attribute("active").set_value(event.start <= now ? 1 : 0);
		if (!event.color.empty()) {
			node.append_attribute("color") = event.color.c_str();
		}
		if (!event.description.empty()) {
			node.append_attribute("description") = event.description.c_str();
		}
	}
}

void appendTibiaExt(pugi::xml_node tsqp)
{
	const int64_t now = static_cast<int64_t>(std::time(nullptr));
	pugi::xml_node ext = tsqp.append_child("tibiaext");
	ext.append_attribute("version") = "1";
	ext.append_attribute("time") = std::to_string(now).c_str();
	appendBoosted(ext, "boostedcreature", getBoostedCreatureLoginEntry());
	appendBoosted(ext, "boostedboss", getBoostedBossLoginEntry());
	appendStoreMount(ext, now);
	appendEventSchedule(ext, now);
}

} // namespace

void ProtocolStatus::onRecvFirstMessage(NetworkMessage& msg)
{
	const uint32_t ip = getIP();
	const int64_t now = OTSYS_TIME();
	const bool whitelisted = ip == 0x0100007F || convertIPToString(ip) == getString(ConfigManager::IP);
	bool blocked = false;
	{
		std::scoped_lock lock(ipConnectMutex);
		if (lastCleanup == 0 || now - lastCleanup >= 60'000) {
			lastCleanup = now;
			const int64_t retention = std::max<int64_t>(60'000, getInteger(ConfigManager::STATUSQUERY_TIMEOUT));
			std::erase_if(ipConnectMap, [now, retention](const auto& pair) {
				return now - pair.second > retention;
			});
		}

		if (!whitelisted) {
			const auto it = ipConnectMap.find(ip);
			blocked = it != ipConnectMap.end() &&
			          now < it->second + getInteger(ConfigManager::STATUSQUERY_TIMEOUT);
		}

		if (!blocked) {
			ipConnectMap[ip] = now;
		}
	}

	if (blocked) {
		disconnect();
		return;
	}

	switch (msg.getByte()) {
		// XML info protocol
		case 0xFF: {
			if (msg.getString(4) == "info") {
				g_dispatcher.addTask([thisPtr = std::static_pointer_cast<ProtocolStatus>(shared_from_this())]() {
					thisPtr->sendStatusString();
				});
				return;
			}
			break;
		}

		// Another ServerInfo protocol
		case 0x01: {
			uint16_t requestedInfo = msg.get<uint16_t>(); // only a Byte is necessary, though we could add new info here
			std::string characterName;
			if (requestedInfo & REQUEST_PLAYER_STATUS_INFO) {
				characterName = msg.getString();
			}
			g_dispatcher.addTask(
			    [=, thisPtr = std::static_pointer_cast<ProtocolStatus>(shared_from_this()),
			     characterName = std::string{characterName}]() { thisPtr->sendInfo(requestedInfo, characterName); });
			return;
		}

		default:
			break;
	}
	disconnect();
}

void ProtocolStatus::sendStatusString()
{
	auto output = OutputMessagePool::getOutputMessage();

	setRawMessages(true);

	pugi::xml_document doc;

	pugi::xml_node decl = doc.prepend_child(pugi::node_declaration);
	decl.append_attribute("version") = "1.0";

	pugi::xml_node tsqp = doc.append_child("tsqp");
	tsqp.append_attribute("version") = "1.0";

	pugi::xml_node serverinfo = tsqp.append_child("serverinfo");
	uint64_t uptime = (OTSYS_TIME() - ProtocolStatus::start) / 1000;
	serverinfo.append_attribute("uptime") = std::to_string(uptime).c_str();
	serverinfo.append_attribute("ip") = getString(ConfigManager::IP).data();
	serverinfo.append_attribute("servername") = getString(ConfigManager::SERVER_NAME).data();
	serverinfo.append_attribute("port") = std::to_string(getInteger(ConfigManager::LOGIN_PORT)).c_str();
	serverinfo.append_attribute("location") = getString(ConfigManager::LOCATION).data();
	serverinfo.append_attribute("url") = getString(ConfigManager::URL).data();
	serverinfo.append_attribute("server") = STATUS_SERVER_NAME;
	serverinfo.append_attribute("version") = STATUS_SERVER_VERSION;
	serverinfo.append_attribute("client") = CLIENT_VERSION_STR;

	pugi::xml_node owner = tsqp.append_child("owner");
	owner.append_attribute("name") = getString(ConfigManager::OWNER_NAME).data();
	owner.append_attribute("email") = getString(ConfigManager::OWNER_EMAIL).data();

	pugi::xml_node players = tsqp.append_child("players");
	constexpr uint32_t maxPlayersPerIp = 4;
	uint32_t onlinePlayers = 0;
	uint32_t uniquePlayers = 0;
	std::unordered_map<uint32_t, uint32_t> playersByIp;

	for (const auto& player : g_game.getPlayers()) {
		if (!player) {
			continue;
		}

		const uint32_t playerIp = player->getIP();
		if (playerIp == 0) {
			continue;
		}

		auto [it, inserted] = playersByIp.try_emplace(playerIp, 0);
		++it->second;
		if (inserted) {
			++uniquePlayers;
		}
		if (it->second <= maxPlayersPerIp) {
			++onlinePlayers;
		}
	}

	players.append_attribute("online").set_value(onlinePlayers);
	players.append_attribute("unique").set_value(uniquePlayers);
	players.append_attribute("max") = std::to_string(getInteger(ConfigManager::MAX_PLAYERS)).c_str();
	players.append_attribute("peak") = std::to_string(g_game.getPlayersRecord()).c_str();

	pugi::xml_node monsters = tsqp.append_child("monsters");
	monsters.append_attribute("total") = std::to_string(g_game.getMonstersOnline()).c_str();

	pugi::xml_node npcs = tsqp.append_child("npcs");
	npcs.append_attribute("total") = std::to_string(g_game.getNpcsOnline()).c_str();

	pugi::xml_node rates = tsqp.append_child("rates");
	rates.append_attribute("experience") = std::to_string(getInteger(ConfigManager::RATE_EXPERIENCE)).c_str();
	rates.append_attribute("skill") = std::to_string(getInteger(ConfigManager::RATE_SKILL)).c_str();
	rates.append_attribute("loot") = std::to_string(getInteger(ConfigManager::RATE_LOOT)).c_str();
	rates.append_attribute("magic") = std::to_string(getInteger(ConfigManager::RATE_MAGIC)).c_str();
	rates.append_attribute("spawn") = std::to_string(getInteger(ConfigManager::RATE_SPAWN)).c_str();

	pugi::xml_node map = tsqp.append_child("map");
	map.append_attribute("name") = getString(ConfigManager::MAP_NAME).data();
	map.append_attribute("author") = getString(ConfigManager::MAP_AUTHOR).data();

	uint32_t mapWidth, mapHeight;
	g_game.getMapDimensions(mapWidth, mapHeight);
	map.append_attribute("width") = std::to_string(mapWidth).c_str();
	map.append_attribute("height") = std::to_string(mapHeight).c_str();

	pugi::xml_node motd = tsqp.append_child("motd");
	motd.text() = getString(ConfigManager::MOTD).data();

	appendTibiaExt(tsqp);

	std::ostringstream ss;
	doc.save(ss, "", pugi::format_raw);

	std::string data = ss.str();
	output->addBytes(data.c_str(), data.size());
	send(output);
	disconnect();
}

void ProtocolStatus::sendInfo(uint16_t requestedInfo, std::string_view characterName)
{
	auto output = OutputMessagePool::getOutputMessage();

	if (requestedInfo & REQUEST_BASIC_SERVER_INFO) {
		output->addByte(0x10);
		output->addString(getString(ConfigManager::SERVER_NAME));
		output->addString(getString(ConfigManager::IP));
		output->addString(std::to_string(getInteger(ConfigManager::LOGIN_PORT)));
	}

	if (requestedInfo & REQUEST_OWNER_SERVER_INFO) {
		output->addByte(0x11);
		output->addString(getString(ConfigManager::OWNER_NAME));
		output->addString(getString(ConfigManager::OWNER_EMAIL));
	}

	if (requestedInfo & REQUEST_MISC_SERVER_INFO) {
		output->addByte(0x12);
		output->addString(getString(ConfigManager::MOTD));
		output->addString(getString(ConfigManager::LOCATION));
		output->addString(getString(ConfigManager::URL));
		output->add<uint64_t>((OTSYS_TIME() - ProtocolStatus::start) / 1000);
	}

	if (requestedInfo & REQUEST_PLAYERS_INFO) {
		output->addByte(0x20);
		output->add<uint32_t>(g_game.getPlayersOnline());
		output->add<uint32_t>(getInteger(ConfigManager::MAX_PLAYERS));
		output->add<uint32_t>(g_game.getPlayersRecord());
	}

	if (requestedInfo & REQUEST_MAP_INFO) {
		output->addByte(0x30);
		output->addString(getString(ConfigManager::MAP_NAME));
		output->addString(getString(ConfigManager::MAP_AUTHOR));
		uint32_t mapWidth, mapHeight;
		g_game.getMapDimensions(mapWidth, mapHeight);
		output->add<uint16_t>(static_cast<uint16_t>(mapWidth));
		output->add<uint16_t>(static_cast<uint16_t>(mapHeight));
	}

	if (requestedInfo & REQUEST_EXT_PLAYERS_INFO) {
		output->addByte(0x21); // players info - online players list

		const auto& players = g_game.getPlayers();
		output->add<uint32_t>(players.size());
		for (const auto& player : players) {
			output->addString(player->getName());
			output->add<uint32_t>(player->getLevel());
		}
	}

	if (requestedInfo & REQUEST_PLAYER_STATUS_INFO) {
		output->addByte(0x22); // players info - online status info of a player
		if (g_game.getPlayerByName(characterName) != nullptr) {
			output->addByte(0x01);
		} else {
			output->addByte(0x00);
		}
	}

	if (requestedInfo & REQUEST_SERVER_SOFTWARE_INFO) {
		output->addByte(0x23); // server software info
		output->addString(STATUS_SERVER_NAME);
		output->addString(STATUS_SERVER_VERSION);
		output->addString(CLIENT_VERSION_STR);
	}
	send(output);
	disconnect();
}
