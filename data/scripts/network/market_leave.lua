local handler = PacketHandler(0xF4)

function handler.onReceive(player, msg)
	Market.removePlayer(player:getGuid())
end

handler:register()
