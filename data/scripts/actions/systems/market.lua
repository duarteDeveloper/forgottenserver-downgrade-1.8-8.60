local MARKET_ITEM_ID = ITEM_MARKET or 12903

local action = Action()

function action.onUse(player, item, fromPosition, target, toPosition, isHotkey)
	local tibiaExt = player.isUsingTibiaExt and player:isUsingTibiaExt()
	if not player:isUsingOtClient() and not tibiaExt then
		player:sendCancelMessage("The market is only available on OTClient.")
		return true
	end

	if not configManager.getBoolean(configKeys.MARKET_SYSTEM_ENABLED) then
		player:sendCancelMessage("The market is disabled.")
		return true
	end

	if not CustomMarket or not CustomMarket.open then
		player:sendCancelMessage("The market is not available.")
		return true
	end

	local depotId = 0
	if player.getLastDepotId then
		local ok, value = pcall(function()
			return player:getLastDepotId()
		end)
		depotId = tonumber(ok and value or nil) or 0
	end

	CustomMarket.open(player, depotId)
	return true
end

action:id(MARKET_ITEM_ID)
action:register()
