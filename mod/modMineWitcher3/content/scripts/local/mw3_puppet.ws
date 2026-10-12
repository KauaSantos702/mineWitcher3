// =============================================================================
// MineWitcher3 - o Geralt como fantoche do jogador do Minecraft (research/03 secao 4).
//
// Posicao: autoridade do MC. O Geralt segue os pes do jogador do MC (os ticks de 20 Hz interpolados
// pela DLL no relogio do jogo, os mesmos de onde sai o olho da camera) com TeleportWithRotation,
// olhando para onde anda (parado: mantem a direcao). So corrige quando a locomocao se afastou mais
// que alguns cm (NeedsCorrection): abaixo disso a animacao anda livre, sem briga a cada frame.
// Animacao: "stick virtual". A locomocao do W3 le o analogico em CalculateInputVector (exploracao)
// e HandleMovement/WouldLikeToMove (player); com o fantoche ligado elas recebem a velocidade do MC
// convertida para o espaco da camera, entao o Geralt anda/corre com as animacoes dele. A deriva
// entre o passo da animacao e o do MC fica abaixo da tolerancia de NeedsCorrection.
// O teclado/mouse do W3 ja nao chegam ao jogo (a DLL os manda ao MC), entao nada mais move o Geralt.
//
// So liga com o MC linkado E depois que o MC confirmou o ultimo teleporte (soltou o hold): antes
// disso o MC esta parado esperando a colisao, e o Geralt tambem. Desliga sozinho em menus, cenas,
// loading, montado (cavalo/barco) e morto; se o W3 mover o Geralt por conta propria (cutscene,
// porta, fast travel) a DLL percebe e teleporta o MC para la.
// =============================================================================

class MW3_Puppet
{
	private var driving : bool;
	private var moving  : bool;
	private var heading : float;
	private var stick   : Vector;	// analogico virtual, espaco da camera, |v| <= 1
	private var stickLen : float;

	// Animar via stick virtual (exec mw3anim 0/1). Desligado: so teleporte (desliza em idle).
	public var animate  : bool;		default animate = true;

	// Velocidade de caminhada do MC (blocos/s) = analogico todo: o Geralt corre.
	private var MC_WALK  : float;	default MC_WALK  = 4.3;
	private var MIN_MOVE : float;	default MIN_MOVE = 0.3;

	// Trava contra "cair do mapa": se os pes do MC ficarem abaixo do chao do W3 sob eles (colisao
	// ainda nao chegou ou veio errada), se nao houver chao nenhum embaixo deles, ou se pularem longe
	// demais num frame (respawn do MC), o Geralt fica onde esta. Bloqueado por mais de RESYNC_AFTER,
	// o script pede a DLL para teleportar o MC de volta para o Geralt (com hold ate a colisao chegar).
	private var groundGroups : array<name>;
	private var BELOW_TOL    : float;	default BELOW_TOL = 2.0;	// abaixo disso: queda real, Geralt para
	private var SNAP_TOL     : float;	default SNAP_TOL  = 2.0;	// ate isso: Geralt vai para o chao do W3
	private var GROUND_DEPTH : float;	default GROUND_DEPTH = 50.0;	// chao procurado ate isso abaixo dos pes
	private var MAX_STEP     : float;	default MAX_STEP = 6.0;		// pes do MC mais longe que isso do Geralt num frame: rejeita
	private var RESYNC_AFTER : float;	default RESYNC_AFTER = 0.5;	// segundos bloqueado ate pedir resync
	private var RESYNC_AGAIN : float;	default RESYNC_AGAIN = 3.0;	// e de novo a cada isso, se continuar bloqueado
	private var blocked      : bool;
	private var blockedSince : float;
	private var lastResync   : float;
	private var MC_DEAD      : int;		default MC_DEAD = 32;		// proto::kMcDead

	// Correcao do fantoche (NeedsCorrection): abaixo disso a locomocao do W3 anda livre.
	private var POS_TOL_MOVE : float;	default POS_TOL_MOVE = 0.08;	// metros no plano, andando
	private var POS_TOL_IDLE : float;	default POS_TOL_IDLE = 0.02;	// metros no plano, parado
	private var Z_TOL        : float;	default Z_TOL = 0.10;
	private var YAW_TOL      : float;	default YAW_TOL = 3.0;		// graus

	public function IsDriving() : bool
	{
		return driving;
	}

	public function IsAnimating() : bool
	{
		return driving && animate;
	}

	public function IsMoving() : bool
	{
		return driving && animate && moving;
	}

	public function GetStick() : Vector
	{
		return stick;
	}

	public function GetStickLength() : float
	{
		return stickLen;
	}

	public function GetHeading() : float
	{
		return heading;
	}

	// Uma vez por frame de gameplay, depois de MW3_LinkConnected() (OnGameCameraPostTick).
	public function Tick( dt : float, menu : bool, loading : bool )
	{
		var feet  : Vector;
		var vel   : Vector;
		var here  : Vector;
		var rot   : EulerAngles;
		var speed : float;

		if( !thePlayer || menu || loading || !MW3_PuppetActive() || !thePlayer.IsAlive() || thePlayer.IsUsingVehicle() || ( MW3_McFlags() & MC_DEAD ) != 0 )
		{
			Stop();
			return;
		}
		if( !driving )
		{
			driving = true;
			heading = thePlayer.GetHeading();
			MW3_Log( "Fantoche: o Minecraft dirige o Geralt" );
		}

		feet  = Vector( MW3_McFeetX(), MW3_McFeetY(), MW3_McFeetZ(), 1.0 );
		vel   = Vector( MW3_McVelX(), MW3_McVelY(), 0.0, 0.0 );
		speed = VecLength2D( vel );
		moving = speed > MIN_MOVE;
		if( moving )
			heading = VecHeading( vel );

		// Stick virtual: a locomocao soma o heading da camera (rawPlayerHeading = camera + stick).
		if( moving && animate )
		{
			stickLen = ClampF( speed / MC_WALK, 0.3, 1.0 );
			stick    = VecFromHeading( AngleDistance( heading, theCamera.GetCameraHeading() ) ) * stickLen;
		}
		else
		{
			stickLen = 0.0;
			stick    = Vector( 0.0, 0.0, 0.0, 0.0 );
		}

		here = thePlayer.GetWorldPosition();
		// Salto impossivel a pe num frame (respawn do MC no spawn do mundo, teleporte do server): o
		// Geralt nao vai junto (research/07 3.2: foi assim que ele acabou no vazio).
		if( VecDistance( here, feet ) > MAX_STEP )
		{
			BlockPuppet( "pes do MC a " + FloatToString( VecDistance( here, feet ) ) + " m do Geralt num frame" );
			return;
		}
		// A colisao mandada ao MC pode ficar alguns decimos acima/abaixo do chao real (research/06):
		// o Geralt pisa no chao do W3 sob os pes do MC, em vez de afundar ou flutuar.
		feet = SnapToGround( feet, here );
		if( IsBelowGround( feet, here ) )
		{
			BlockPuppet( "pes do MC (z=" + FloatToString( feet.Z ) + ") abaixo do chao do W3 ou sem chao embaixo" );
			return;
		}
		if( blocked )
			MW3_Log( "Fantoche: MC de volta acima do chao" );
		blocked = false;

		// Sem brigar com a locomocao: o stick virtual ja anda o Geralt quase no passo do MC, entao so
		// corrige quando o erro passa de alguns cm (ou do giro); abaixo disso a animacao segue livre.
		// Os pes ja vem interpolados no relogio do jogo (DLL), entao a correcao e pequena e suave.
		rot  = thePlayer.GetWorldRotation();
		if( NeedsCorrection( here, feet, rot.Yaw ) )
		{
			rot.Pitch = 0.0;
			rot.Roll  = 0.0;
			rot.Yaw   = heading;
			thePlayer.TeleportWithRotation( feet, rot );
		}
		// Referencia da DLL para "o W3 moveu o Geralt sozinho" (reteleporta o MC).
		MW3_PuppetMoved( feet.X, feet.Y, feet.Z );
	}

	// Teleporta o Geralt so quando a locomocao dele se afastou do MC: com o stick virtual andando, ate
	// POS_TOL_MOVE no plano (a animacao anda quase no passo do MC); parado, POS_TOL_IDLE (o idle nao
	// pode deslizar); Z ate Z_TOL (o chao ja vem do SnapToGround); giro ate YAW_TOL (a locomocao gira
	// o Geralt sozinha para o heading do stick). Sem animacao (mw3anim 0) so o teleporte o move.
	private function NeedsCorrection( here : Vector, feet : Vector, yaw : float ) : bool
	{
		var flat : Vector;
		var tol  : float;

		if( !animate )
			return VecDistance( here, feet ) > 0.005 || AbsF( AngleDistance( yaw, heading ) ) > 0.5;
		flat   = here;
		flat.Z = feet.Z;
		if( moving )
			tol = POS_TOL_MOVE;
		else
			tol = POS_TOL_IDLE;
		if( VecDistance( flat, feet ) > tol )
			return true;
		if( AbsF( here.Z - feet.Z ) > Z_TOL )
			return true;
		return AbsF( AngleDistance( yaw, heading ) ) > YAW_TOL;
	}

	// O Geralt fica parado; bloqueado por mais de RESYNC_AFTER, pede a DLL para trazer o MC de volta.
	private function BlockPuppet( why : string )
	{
		var now : float;

		now = theGame.GetEngineTimeAsSeconds();
		if( !blocked )
		{
			MW3_Log( "Fantoche: " + why + "; Geralt parado ate o MC voltar" );
			blocked      = true;
			blockedSince = now;
			lastResync   = -1000.0;
			return;
		}
		if( now - blockedSince >= RESYNC_AFTER && now - lastResync >= RESYNC_AGAIN )
		{
			lastResync = now;
			MW3_Log( "Fantoche: bloqueado ha " + FloatToString( now - blockedSince ) + " s; pedindo resync do MC para o Geralt" );
			MW3_RequestResync();
		}
	}

	// Mesmos grupos da colisao mandada ao MC (mw3_collision.ws Setup): o que o MC pisa, o Geralt pisa.
	private function SetupGroups()
	{
		if( groundGroups.Size() > 0 )
			return;
		groundGroups.PushBack( 'Terrain' );
		groundGroups.PushBack( 'Static' );
		groundGroups.PushBack( 'Foliage' );
		groundGroups.PushBack( 'Fence' );
		groundGroups.PushBack( 'Door' );
		groundGroups.PushBack( 'Platforms' );
		groundGroups.PushBack( 'Boat' );
		groundGroups.PushBack( 'BoatDocking' );
		groundGroups.PushBack( 'DeniedArea' );
	}

	// Chao do W3 sob (x, y) dos pes do MC, procurado ate SNAP_TOL acima/abaixo deles.
	private function SnapToGround( feet : Vector, here : Vector ) : Vector
	{
		var hitPos, hitNormal : Vector;

		SetupGroups();
		if( !theGame.GetWorld().StaticTrace( Vector( feet.X, feet.Y, feet.Z + SNAP_TOL, 1.0 ), Vector( feet.X, feet.Y, feet.Z - SNAP_TOL, 1.0 ), hitPos, hitNormal, groundGroups ) )
			return feet;
		// Raio nasceu dentro de um solido (teto baixo): nao mexe.
		if( hitPos.Z >= feet.Z + SNAP_TOL - 0.01 )
			return feet;
		feet.Z = hitPos.Z;
		return feet;
	}

	// Chao do W3 sob os pes do MC: raio de cima (acima do Geralt e do MC) para baixo. Sem chao nenhum
	// ate GROUND_DEPTH abaixo dos pes (vazio, fora do mapa, ainda nao carregado) tambem e inseguro.
	private function IsBelowGround( feet : Vector, here : Vector ) : bool
	{
		var hitPos, hitNormal : Vector;
		var top : float;

		SetupGroups();
		top = MaxF( feet.Z, here.Z ) + 2.0;
		if( !theGame.GetWorld().StaticTrace( Vector( feet.X, feet.Y, top, 1.0 ), Vector( feet.X, feet.Y, feet.Z - GROUND_DEPTH, 1.0 ), hitPos, hitNormal, groundGroups ) )
			return true;
		return feet.Z < hitPos.Z - BELOW_TOL;
	}

	public function Stop()
	{
		blocked = false;
		if( !driving )
			return;
		driving  = false;
		moving   = false;
		stickLen = 0.0;
		stick    = Vector( 0.0, 0.0, 0.0, 0.0 );
		MW3_Log( "Fantoche: desligado" );
	}
}

function MW3_PuppetOrNull() : MW3_Puppet
{
	return MW3_Get().GetPuppet();
}

// ---- stick virtual -------------------------------------------------------------------------------
// Exploracao: o analogico que a locomocao direta le (locomotionDirectController.ws:695).
@wrapMethod(CR4LocomotionPlayerControllerScript)
function CalculateInputVector() : Vector
{
	var p : MW3_Puppet;

	p = MW3_PuppetOrNull();
	if( p && p.IsAnimating() )
		return p.GetStick();
	return wrappedMethod();
}

// Variavel 'playerWouldLikeToMove' do behavior (r4Player.ws:1429).
@wrapMethod(CR4Player)
function WouldLikeToMove() : bool
{
	var p : MW3_Puppet;

	p = MW3_PuppetOrNull();
	if( p && p.IsAnimating() )
		return p.IsMoving();
	return wrappedMethod();
}

// Velocidade/direcao "cruas" do player e 'lAxisLength' (player.ws:574): o mesmo que o analogico daria.
@wrapMethod(CR4Player)
function HandleMovement( deltaTime : float )
{
	var p : MW3_Puppet;

	wrappedMethod( deltaTime );
	p = MW3_PuppetOrNull();
	if( !p || !p.IsAnimating() )
		return;
	if( p.IsMoving() )
	{
		rawPlayerSpeed   = 0.9 * p.GetStickLength();
		rawPlayerHeading = p.GetHeading();
		cachedRawPlayerHeading = rawPlayerHeading;
		rawPlayerAngle   = AngleDistance( rawPlayerHeading, GetHeading() );
		SetBehaviorVariable( 'lAxisLength', p.GetStickLength() );
	}
	else
	{
		rawPlayerSpeed = 0.0;
		SetBehaviorVariable( 'lAxisLength', 0.0 );
	}
}

// O MC calcula o dano de queda dele; o W3 nao soma outro por cima (os pulos do MC sao teleportes aqui).
@wrapMethod(CR4Player)
function ApplyFallingDamage( heightDiff : float, optional reducing : bool ) : float
{
	var p : MW3_Puppet;

	p = MW3_PuppetOrNull();
	if( p && p.IsDriving() )
		return 0.0;
	return wrappedMethod( heightDiff, reducing );
}

exec function mw3anim( on : int )
{
	var p : MW3_Puppet;

	p = MW3_PuppetOrNull();
	if( p )
		p.animate = on != 0;
}
