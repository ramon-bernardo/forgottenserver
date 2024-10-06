local spell = Spell(SPELL_INSTANT)

function spell.onCastSpell(creature, variant)
	return creature:conjureItem(0, 2546, 8, CONST_ME_MAGIC_BLUE)
end

spell:group("support")
spell:id(49)
spell:name("Conjure Explosive Arrow")
spell:words("exevo con flam")
spell:level(25)
spell:mana(290)
spell:soul(3)
spell:isPremium(true)
spell:isAggressive(false)
spell:isSelfTarget(true)
spell:cooldown(2000)
spell:groupCooldown(2000)
spell:vocation("paladin;true", "royal paladin;true")
spell:register()