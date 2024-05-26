local handler = PacketHandler(0xF4)

function handler.onReceive(player, msg)
	local browseId = msg:getByte()
	if browseId == MARKET_OWN_OFFERS then

	elseif browseId == MARKET_OWN_HISTORY then

	else
		local spriteId = msg:getU16()
	end
end

handler:register()
