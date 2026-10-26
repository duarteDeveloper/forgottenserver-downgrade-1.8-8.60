// Copyright 2026 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#ifndef FS_STORE_PROTOCOL_H
#define FS_STORE_PROTOCOL_H

#include "networkmessage.h"
#include "store/store_types.h"

#include <cstdint>

/// Named constants for the custom 8.60 store wire protocol.
/// These are consumed by AstraClient / OTClient and MUST NOT change.
namespace StoreProtocol {

enum class ClientOpcode : uint8_t
{
	Transfer = 0xF8,
	History = 0xFA,
	Open = 0xFB,
	Buy = 0xFC,
};

/// The single server → client opcode for all store responses.
inline constexpr uint8_t ServerOpcode = 0xFD;

enum class ResponseType : uint8_t
{
	Error = 0x00,
	Catalog = 0x01,
	Success = 0x02,
	History = 0x03,
	CatalogChunk = 0x04,
};

inline constexpr uint8_t CatalogChunkStart = 1 << 0;
inline constexpr uint8_t CatalogChunkEnd = 1 << 1;
inline constexpr size_t CatalogChunkTargetSize = 48 * 1024;

/// TibiaExtendedClientLibrary (the CipSoft 8.60 client with its ddraw.dll).
/// Its Open request starts with this hello, which turns the store protocol on
/// for the session. The 8.60 receive buffer holds 16394 bytes per network
/// message, so its catalog always goes in small chunks and a store message
/// never shares an output message that would grow past TibiaExtMaxMessageBody.
inline constexpr uint32_t TibiaExtHelloMagic = 0x54584554; // "TEXT"
inline constexpr uint8_t TibiaExtHelloVersion = 1;
/// Same hello with version 2, sent by the DLL right after login: turns the
/// protocols on (store and market) without opening the store.
inline constexpr uint8_t TibiaExtHelloOnlyVersion = 2;
inline constexpr size_t TibiaExtCatalogChunkTargetSize = 12 * 1024;
inline constexpr size_t TibiaExtMaxMessageBody = 15000;

[[nodiscard]] constexpr bool shouldUseLegacyCatalog(size_t estimatedSize, bool supportsCatalogChunks,
                                                    size_t chunkTargetSize = CatalogChunkTargetSize) noexcept
{
	return estimatedSize <= chunkTargetSize ||
	       (!supportsCatalogChunks && estimatedSize <= NetworkMessage::MAX_PROTOCOL_BODY_LENGTH);
}

inline void addCatalogChunkHeader(NetworkMessage& msg, uint8_t flags, uint32_t coins,
                                  uint16_t categoryCount, uint16_t categoryPartCount)
{
	msg.addByte(ServerOpcode);
	msg.addByte(static_cast<uint8_t>(ResponseType::CatalogChunk));
	msg.addByte(flags);
	msg.add<uint32_t>(coins);
	msg.add<uint16_t>(categoryCount);
	msg.add<uint16_t>(categoryPartCount);
}

[[nodiscard]] constexpr StoreHighlightState effectiveHighlightState(StoreHighlightState state,
                                                                    uint32_t validUntilTimestamp,
                                                                    uint32_t nowTimestamp) noexcept
{
	if (storeHighlightHasExpiration(state) && validUntilTimestamp != 0 &&
	    validUntilTimestamp <= nowTimestamp) {
		return StoreHighlightState::None;
	}
	return state;
}

inline void addCategoryHighlight(NetworkMessage& msg, bool enabled, StoreHighlightState state)
{
	if (enabled) {
		msg.addByte(static_cast<uint8_t>(state));
	}
}

inline void addOfferPrices(NetworkMessage& msg, bool includeBasePrice, uint32_t effectivePrice,
                           uint32_t basePrice)
{
	msg.add<uint32_t>(effectivePrice);
	if (includeBasePrice) {
		msg.add<uint32_t>(basePrice);
	}
}

inline void addOfferHighlight(NetworkMessage& msg, bool enabled, StoreHighlightState state,
                              uint32_t validUntilTimestamp, uint32_t nowTimestamp)
{
	if (!enabled) {
		return;
	}

	const auto effectiveState = effectiveHighlightState(state, validUntilTimestamp, nowTimestamp);
	msg.addByte(static_cast<uint8_t>(effectiveState));
	if (storeHighlightHasExpiration(effectiveState)) {
		msg.add<uint32_t>(validUntilTimestamp);
	}
}

} // namespace StoreProtocol

#endif // FS_STORE_PROTOCOL_H
