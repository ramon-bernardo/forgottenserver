Market = {}
Market.__index = Market

do
	local players = {}

	function Market.getPlayers() return players end

	function Market.addPlayer(playerGuid)
		players[playerGuid] = os.time()
	end

	function Market.removePlayer(playerGuid)
		players[playerGuid] = nil
	end

	function Market.hasPlayer(playerGuid)
		return players[playerGuid] ~= nil
	end
end

do
	function Market.browseOwnOffers(player)
		local playerGuid = player:getGuid()
		if not Market.hasPlayer(playerGuid) then
			return
		end

		
	end	
end