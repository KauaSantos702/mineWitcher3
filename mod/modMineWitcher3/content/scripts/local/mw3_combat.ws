// =============================================================================
// MineWitcher3 - combate entre o jogador do Minecraft e os atores do W3 (native v6).
// Porte do SkyCraft skse/src/Combat.cpp (WriteActorTable, ApplyHit, BridgePlayerDamage, KillPlayer;
// Copyright (c) 2026 chasmlol, MIT License, ver THIRD-PARTY-NOTICES.md).
//
// So age com o Minecraft linkado (MW3_CombatLinked); sem ele o jogo fica exatamente vanilla.
//
// 1. W3 -> MC, atores: a cada ENUM_PERIOD s o script procura os atores vivos e atacaveis perto do
//    Geralt (FindGameplayEntitiesInSphere, RANGE m, ate MAX_ACTORS); a cada SEND_PERIOD s (20 Hz)
//    manda posicao, yaw, capsula, vida e hostilidade de cada um (MW3_ActorAdd). O MC cria um
//    WitcherActorEntity invisivel para cada um: os cliques do MC passam a acertar os NPCs.
//    Id estavel: tabela do proprio script (CActor -> int), nao o GetGuidHash (0/colisoes em
//    entidades spawnadas por community).
// 2. MC -> W3, golpes: MW3_PollHit drena os kEvHitActor; cada golpe vira um W3DamageAction com o
//    thePlayer como atacante, pelo theGame.damageMgr.ProcessAction (o mesmo caminho do
//    btTaskDettlaffDash.ws:58-62 e npc.ws:2989-3004): reacao/stagger, morte, XP (ShouldGiveExp exige
//    thePlayer), loot e quests como vanilla.
// 3. W3 -> MC, dano no Geralt: o OnTakeDamage do W3PlayerWitcher (embrulhado abaixo) zera a perda de
//    vida do Geralt e manda o dano ao MC (kInHurt), convertido pela fracao da vida maxima do Geralt.
//    A vida e a armadura do MC decidem; a morte do MC mata o Geralt (MW3_PollMcDeath em mw3_main.ws,
//    com a fonte 'MineWitcher3', que este filtro deixa passar).
// =============================================================================

class MW3_Combat
{
	// ---- ajuste (exec mw3hitscale / mw3hurtscale / mw3combatlog) ---------------------------------
	// Golpe do MC -> dano do W3: um inimigo comum do nivel do Geralt aguenta MC_HEALTH pontos de dano
	// do MC (como um zumbi: espada de diamante 7 -> 3 golpes, punho 1 -> 20), qualquer que seja a vida
	// dele no W3. Chefes e monstros enormes aguentam mais; cada nivel acima do Geralt +LEVEL_STEP.
	public var hitScale   : float;	default hitScale   = 1.0;
	// Dano no Geralt -> dano no MC: a fracao da vida maxima do Geralt vezes os 20 pontos de vida do MC.
	public var hurtScale  : float;	default hurtScale  = 1.0;
	public var verbose    : bool;	default verbose    = true;

	private var MC_HEALTH   : float;	default MC_HEALTH   = 20.0;
	private var BOSS_MULT   : float;	default BOSS_MULT   = 6.0;
	private var HUGE_MULT   : float;	default HUGE_MULT   = 2.5;
	private var LEVEL_STEP  : float;	default LEVEL_STEP  = 0.1;
	private var LEVEL_FLOOR : float;	default LEVEL_FLOOR = 0.5;
	private var MC_PLAYER_HEALTH : float;	default MC_PLAYER_HEALTH = 20.0;

	private var RANGE        : float;	default RANGE        = 48.0;
	private var MAX_ACTORS   : int;		default MAX_ACTORS   = 64;
	private var ENUM_PERIOD  : float;	default ENUM_PERIOD  = 0.25;
	private var SEND_PERIOD  : float;	default SEND_PERIOD  = 0.05;
	private var FORGET_AFTER : float;	default FORGET_AFTER = 10.0;	// id some depois disso sem ver o ator
	private var DOT_FLUSH    : float;	default DOT_FLUSH    = 0.5;		// dano continuo (sangramento, veneno, fogo) agrupado

	// proto::ActorFlags / HitFlags / HitWeapon / HurtKind / HurtFlags (protocol/mw3_protocol.h)
	private var ACTOR_HOSTILE   : int;	default ACTOR_HOSTILE   = 1;
	private var ACTOR_ESSENTIAL : int;	default ACTOR_ESSENTIAL = 4;
	private var ACTOR_COMBAT    : int;	default ACTOR_COMBAT    = 8;
	private var HIT_CRITICAL    : int;	default HIT_CRITICAL    = 1;
	private var HIT_PROJECTILE  : int;	default HIT_PROJECTILE  = 2;
	private var WEAPON_ARROW    : int;	default WEAPON_ARROW    = 5;
	private var HURT_MELEE      : int;	default HURT_MELEE      = 0;
	private var HURT_PROJECTILE : int;	default HURT_PROJECTILE = 1;
	private var HURT_MAGIC      : int;	default HURT_MAGIC      = 2;
	private var HURT_OTHER      : int;	default HURT_OTHER      = 3;
	private var HURT_BLOCKED    : int;	default HURT_BLOCKED    = 1;
	private var HURT_HEAVY      : int;	default HURT_HEAVY      = 2;

	// Ids estaveis (paralelos).
	private var knownActors : array<CActor>;
	private var knownIds    : array<int>;
	private var knownSeen   : array<float>;
	private var nextId      : int;		default nextId = 1;

	// Atores da ultima enumeracao + o que nao muda a cada envio (paralelos).
	private var curActors : array<CActor>;
	private var curIds    : array<int>;
	private var curRadius : array<float>;
	private var curHeight : array<float>;
	private var curLevel  : array<int>;
	private var curName   : array<string>;

	private var active    : bool;
	private var nextEnum  : float;
	private var nextSend  : float;

	private var dotDamage   : float;
	private var dotAttacker : int;
	private var dotSince    : float;

	private var hitsApplied : int;
	private var hurtsSent   : int;

	// Uma vez por frame de gameplay (MW3_Manager.SendLinkState).
	public function Tick( menu : bool, loading : bool )
	{
		var now : float;

		if( !thePlayer || !MW3_CombatLinked() )
		{
			Deactivate();
			return;
		}
		now = theGame.GetEngineTimeAsSeconds();
		if( !active )
		{
			active = true;
			nextEnum = 0;
			nextSend = 0;
			MW3_Log( "Combate: Minecraft linkado; atores do W3 vao para o MC, golpes do MC valem no W3" );
		}

		if( loading )
		{
			// Sem atores durante o loading; golpes velhos nao caem depois.
			DiscardHits();
			curActors.Clear();
			MW3_ActorsClear();
			return;
		}

		ApplyPendingHits();

		if( now >= nextEnum )
		{
			nextEnum = now + ENUM_PERIOD;
			Enumerate( now );
		}
		if( now >= nextSend )
		{
			nextSend = now + SEND_PERIOD;
			SendActors();
		}
		if( dotDamage > 0 && now - dotSince >= DOT_FLUSH )
			FlushDot();
	}

	public function SetHitScale( s : float )	{ hitScale = s; }
	public function SetHurtScale( s : float )	{ hurtScale = s; }
	public function SetVerbose( on : bool )	{ verbose = on; }

	private function DiscardHits()
	{
		var n : int;

		n = 0;
		while( n < 256 && MW3_PollHit() != 0 )
			n += 1;
	}

	private function Deactivate()
	{
		if( !active )
			return;
		active = false;
		curActors.Clear();
		curIds.Clear();
		curRadius.Clear();
		curHeight.Clear();
		curLevel.Clear();
		curName.Clear();
		knownActors.Clear();
		knownIds.Clear();
		knownSeen.Clear();
		dotDamage = 0;
		MW3_ActorsClear();
		MW3_Log( "Combate: Minecraft desligado; combate do W3 volta ao normal (golpes aplicados " + hitsApplied + ", danos enviados " + hurtsSent + ")" );
	}

	// ------------------------------------------------------------ ids estaveis

	private function IdOf( actor : CActor, create : bool, now : float ) : int
	{
		var i : int;

		if( !actor )
			return 0;
		for( i = 0; i < knownActors.Size(); i += 1 )
		{
			if( knownActors[i] == actor )
			{
				if( create )
					knownSeen[i] = now;
				return knownIds[i];
			}
		}
		if( !create )
			return 0;
		knownActors.PushBack( actor );
		knownIds.PushBack( nextId );
		knownSeen.PushBack( now );
		nextId += 1;
		if( nextId > 2000000000 )
			nextId = 1;
		return knownIds[ knownIds.Size() - 1 ];
	}

	private function ActorById( id : int ) : CActor
	{
		var i : int;

		for( i = 0; i < knownIds.Size(); i += 1 )
		{
			if( knownIds[i] == id )
				return knownActors[i];
		}
		return NULL;
	}

	private function ForgetOld( now : float )
	{
		var i : int;

		for( i = knownActors.Size() - 1; i >= 0; i -= 1 )
		{
			if( !knownActors[i] || now - knownSeen[i] > FORGET_AFTER )
			{
				knownActors.Erase( i );
				knownIds.Erase( i );
				knownSeen.Erase( i );
			}
		}
	}

	// ------------------------------------------------------------ 1. atores -> MC

	private function Enumerate( now : float )
	{
		var ents  : array<CGameplayEntity>;
		var actor : CActor;
		var npc   : CNewNPC;
		var mac   : CMovingPhysicalAgentComponent;
		var i     : int;
		var r, h  : float;

		curActors.Clear();
		curIds.Clear();
		curRadius.Clear();
		curHeight.Clear();
		curLevel.Clear();
		curName.Clear();

		FindGameplayEntitiesInSphere( ents, thePlayer.GetWorldPosition(), RANGE, MAX_ACTORS, '', FLAG_ExcludePlayer + FLAG_OnlyAliveActors );
		for( i = 0; i < ents.Size(); i += 1 )
		{
			actor = (CActor)ents[i];
			if( !actor || actor == thePlayer || !actor.IsAlive() )
				continue;
			// Como o damageManager.ws:28: o que o Geralt nao pode atacar nao vira proxy (os cliques do
			// MC atravessam e acertam os blocos atras).
			if( !actor.IsAttackableByPlayer() || !actor.GetVisibility() )
				continue;
			npc = (CNewNPC)actor;
			if( npc && npc.IsHorse() )
				continue;	// a Roach (e cavalos em geral) nao viram alvo de um clique perdido

			mac = (CMovingPhysicalAgentComponent)actor.GetMovingAgentComponent();
			r = 0.4;
			h = 1.8;
			if( mac )
			{
				r = mac.GetCapsuleRadius();
				h = mac.GetCapsuleHeight();
			}
			// Monstros grandes tem capsula de navegacao fina; o raio do ator cobre melhor o corpo.
			r = MaxF( r, MinF( actor.GetRadius(), 3.0 ) );
			if( r <= 0.05 )
				r = 0.4;
			if( h <= 0.2 )
				h = 1.8;
			h = MaxF( h, r * 1.2 );

			curActors.PushBack( actor );
			curIds.PushBack( IdOf( actor, true, now ) );
			curRadius.PushBack( r );
			curHeight.PushBack( h );
			if( npc )
				curLevel.PushBack( npc.GetLevel() );
			else
				curLevel.PushBack( 0 );
			curName.PushBack( actor.GetDisplayName() );
		}
		ForgetOld( now );
	}

	private function SendActors()
	{
		var i, flags : int;
		var actor : CActor;
		var pos   : Vector;
		var rot   : EulerAngles;

		MW3_ActorsBegin();
		for( i = 0; i < curActors.Size(); i += 1 )
		{
			actor = curActors[i];
			if( !actor || !actor.IsAlive() )
				continue;
			flags = 0;
			if( actor.GetAttitude( thePlayer ) == AIA_Hostile )
				flags = flags | ACTOR_HOSTILE;
			if( actor.IsInCombat() )
				flags = flags | ACTOR_COMBAT;
			if( actor.IsImmortal() || actor.IsInvulnerable() )
				flags = flags | ACTOR_ESSENTIAL;
			pos = actor.GetWorldPosition();
			rot = actor.GetWorldRotation();	// campo de struct so pode ser lido de uma variavel
			MW3_ActorAdd( curIds[i], pos.X, pos.Y, pos.Z, rot.Yaw, curRadius[i], curHeight[i],
				actor.GetHealth(), actor.GetMaxHealth(), flags, curLevel[i], curName[i] );
		}
		MW3_ActorsCommit();
	}

	// ------------------------------------------------------------ 2. golpes do MC -> W3

	private function ApplyPendingHits()
	{
		var id, n : int;
		var actor : CActor;

		n = 0;
		id = MW3_PollHit();
		while( id != 0 && n < 32 )
		{
			n += 1;
			actor = ActorById( id );
			if( actor && actor.IsAlive() )
				ApplyHit( actor, MW3_HitDamage(), MW3_HitFlags(), MW3_HitWeapon(), MW3_HitPush() );
			else if( verbose )
				MW3_Log( "Combate: golpe do MC no ator " + id + ", que nao existe mais" );
			id = MW3_PollHit();
		}
	}

	private function IsBoss( actor : CActor ) : bool
	{
		var stats : CCharacterStats;

		if( actor.HasAbility( 'Boss' ) || actor.HasAbility( 'SkillBoss' ) || actor.HasAbility( 'MonsterMHBoss' ) || (W3MonsterHuntNPC)actor )
			return true;
		stats = actor.GetCharacterStats();
		return stats && stats.HasAbilityWithTag( 'Boss' );
	}

	// Quanto dano do MC este ator aguenta (o "zumbi" de MC_HEALTH, mais forte se chefe/enorme/nivel alto).
	private function McEquivalentHealth( actor : CActor ) : float
	{
		var eq, lv : float;
		var npc : CNewNPC;

		eq = MC_HEALTH;
		if( IsBoss( actor ) )
			eq *= BOSS_MULT;
		else if( actor.IsHuge() )
			eq *= HUGE_MULT;
		npc = (CNewNPC)actor;
		if( npc )
		{
			lv = (float)( npc.GetLevel() - thePlayer.GetLevel() );
			eq *= MaxF( LEVEL_FLOOR, 1.0 + LEVEL_STEP * lv );
		}
		return eq;
	}

	public function ApplyHit( victim : CActor, mcDamage : float, flags : int, weapon : int, push : float )
	{
		var action  : W3DamageAction;
		var maxHp, dmg, before : float;
		var crit, ranged, strong : bool;
		var hrt     : EHitReactionType;

		maxHp = victim.GetMaxHealth();
		if( maxHp <= 0 || mcDamage <= 0 )
			return;
		crit   = ( flags & HIT_CRITICAL ) != 0;
		ranged = ( flags & HIT_PROJECTILE ) != 0 || weapon == WEAPON_ARROW;
		// Golpe correndo / encantamento Knockback (forca > 0.45) e crits derrubam no stagger; os golpes
		// comuns so fazem a reacao leve (o ritmo de ataque do MC travaria tudo em stagger, SkyCraft ApplyHit).
		strong = push > 0.45;
		dmg = mcDamage * maxHp / McEquivalentHealth( victim ) * hitScale;
		if( crit || strong )
			hrt = EHRT_Heavy;
		else
			hrt = EHRT_Light;

		before = victim.GetHealth();
		action = new W3DamageAction in this;
		// Initialize(att, vict, causer, src, hitReaction, powerStat, isMelee, isRanged, isWitcherSign, isEnvironment)
		action.Initialize( thePlayer, victim, NULL, "MineWitcher3Hit", hrt, CPS_Undefined, !ranged, ranged, false, false );
		// Dano direto: o valor ja e uma fracao da vida do alvo (sem poder de ataque do Geralt nem
		// resistencias do W3 em cima; a imortalidade de quest continua valendo).
		action.AddDamage( theGame.params.DAMAGE_NAME_DIRECT, dmg );
		if( crit )
			action.SetCriticalHit();
		if( strong )
			action.AddEffectInfo( EET_Stagger );
		theGame.damageMgr.ProcessAction( action );
		delete action;

		hitsApplied += 1;
		if( verbose )
			MW3_Log( "Combate: MC acertou " + victim.GetDisplayName() + " (" + FloatToString( mcDamage ) + " MC -> " + FloatToString( dmg ) + " W3; vida "
				+ FloatToString( before ) + " -> " + FloatToString( victim.GetHealth() ) + " de " + FloatToString( maxHp ) + ")" );
	}

	// ------------------------------------------------------------ 3. dano no Geralt -> MC

	// Chamado pelo OnTakeDamage do W3PlayerWitcher antes do original: zera a perda de vida e encaminha.
	public function OnPlayerDamage( action : W3DamageAction )
	{
		var vit, maxVit, mc, raw : float;
		var dodged : bool;
		var kind, flags, attackerId : int;
		var src    : string;
		var attack : W3Action_Attack;

		if( !action || !MW3_CombatLinked() )
			return;
		src = action.GetBuffSourceName();
		if( src == "MineWitcher3" )
			return;	// a morte do jogador do MC (thePlayer.Kill('MineWitcher3', true) em mw3_main.ws)

		vit = action.processedDmg.vitalityDamage;
		raw = action.GetDamageValueTotal();
		attack = (W3Action_Attack)action;
		dodged = action.WasDodged() || ( attack && ( attack.IsParried() || attack.IsCountered() ) );
		if( verbose )
			MW3_Log( "Combate: dano no Geralt: processado " + FloatToString( vit ) + ", bruto " + FloatToString( raw )
				+ ", esquiva/aparo " + MW3_YesNo( dodged ) + ", invulneravel " + MW3_YesNo( thePlayer.IsInvulnerable() ) + ", imortal " + MW3_YesNo( thePlayer.IsImmortal() )
				+ ", fonte '" + src + "'" );
		// O Geralt nao perde vida no W3 enquanto o MC decide (SkyCraft: SetEssential + ponte de dano).
		action.processedDmg.vitalityDamage = 0;
		action.processedDmg.essenceDamage = 0;
		// Campo de 2026-10-12: o W3 entrega processado 0 nos golpes no Geralt. Sem esquiva/aparo,
		// usa o dano bruto do golpe (antes das resistencias do W3; a armadura do MC reduz depois).
		if( vit <= 0 && !dodged )
			vit = raw;
		if( vit <= 0 )
			return;
		// Queda e afogamento o proprio MC simula (fisica e agua dele).
		if( src == "FallingDamage" || (W3Effect_Drowning)action.causer )
			return;
		maxVit = thePlayer.GetStatMax( BCS_Vitality );
		if( maxVit <= 0 )
			return;
		mc = vit / maxVit * MC_PLAYER_HEALTH * hurtScale;
		attackerId = IdOf( (CActor)action.attacker, false, 0.0 );

		if( action.IsDoTDamage() )
		{
			// Sangramento, veneno, fogo: pingos por frame, mandados juntos a cada DOT_FLUSH s.
			if( dotDamage <= 0 )
				dotSince = theGame.GetEngineTimeAsSeconds();
			dotDamage += mc;
			if( attackerId != 0 )
				dotAttacker = attackerId;
			return;
		}

		if( action.IsActionRanged() )
			kind = HURT_PROJECTILE;
		else if( action.IsActionWitcherSign() )
			kind = HURT_MAGIC;
		else if( action.IsActionMelee() )
			kind = HURT_MELEE;
		else
			kind = HURT_OTHER;
		flags = 0;
		if( action.GetHitReactionType() == EHRT_Heavy )
			flags = flags | HURT_HEAVY;
		attack = (W3Action_Attack)action;
		if( attack && ( attack.IsParried() || attack.IsCountered() ) )
			flags = flags | HURT_BLOCKED;

		if( MW3_SendPlayerHurt( kind, mc, attackerId, flags ) )
		{
			hurtsSent += 1;
			if( verbose )
				MW3_Log( "Combate: o W3 acertou o Geralt (" + FloatToString( vit ) + " de " + FloatToString( maxVit ) + " -> " + FloatToString( mc )
					+ " MC, tipo " + kind + ", atacante " + attackerId + ", fonte '" + src + "')" );
		}
	}

	private function FlushDot()
	{
		if( MW3_SendPlayerHurt( HURT_MAGIC, dotDamage, dotAttacker, 0 ) )
			hurtsSent += 1;
		dotDamage = 0;
		dotAttacker = 0;
	}
}

// O dano no Geralt passa por aqui (tambem montado: horseRiding.ws:809 chama o do pai).
// O parser so aceita 'function' depois de @wrapMethod, mesmo quando o original e um 'event';
// sem tipo de retorno explicito (eventos sao sempre bool).
@wrapMethod(W3PlayerWitcher)
function OnTakeDamage( action : W3DamageAction )
{
	var res    : bool;
	var combat : MW3_Combat;

	combat = MW3_Get().GetCombat();
	if( combat )
		combat.OnPlayerDamage( action );
	res = wrappedMethod( action );
	return res;
}

// Console: escalas do combate (1 = padrao).
exec function mw3hitscale( s : float )
{
	var c : MW3_Combat;

	c = MW3_Get().GetCombat();
	if( c )
		c.SetHitScale( s );
}

exec function mw3hurtscale( s : float )
{
	var c : MW3_Combat;

	c = MW3_Get().GetCombat();
	if( c )
		c.SetHurtScale( s );
}

exec function mw3combatlog( on : bool )
{
	var c : MW3_Combat;

	c = MW3_Get().GetCombat();
	if( c )
		c.SetVerbose( on );
}

function MW3_YesNo( b : bool ) : string
{
	if( b )
		return "sim";
	return "nao";
}
