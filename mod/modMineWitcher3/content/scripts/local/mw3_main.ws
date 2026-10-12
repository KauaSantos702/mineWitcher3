// =============================================================================
// MineWitcher3 - mecanicas estilo Minecraft dentro do mundo de The Witcher 3
// Requer The Witcher 3 next-gen (4.04+) por causa das anotacoes @addField/@wrapMethod.
// =============================================================================

enum EMW3GameMode
{
	MW3_Survival,
	MW3_Creative
}

// O gerenciador vive no CR4Game para sobreviver a troca de input handler do player.
@addField(CR4Game)
var mw3 : MW3_Manager;

// theGame e const: o campo so pode ser atribuido de dentro de um metodo do proprio CR4Game.
@addMethod(CR4Game)
function MW3_GetManager() : MW3_Manager
{
	if( !mw3 )
	{
		mw3 = new MW3_Manager in this;
	}
	return mw3;
}

function MW3_Get() : MW3_Manager
{
	return theGame.MW3_GetManager();
}

// CPlayerInput.Initialize roda ao spawnar, ao carregar save e ao trocar de handler.
@wrapMethod(CPlayerInput)
function Initialize(isFromLoad : bool, optional previousInput : CPlayerInput)
{
	wrappedMethod(isFromLoad, previousInput);
	MW3_Get().Init();
}

class MW3_Manager
{
	private var mode         : EMW3GameMode;
	private var blocks       : array<CEntity>;
	private var blockPaths   : array<string>;
	private var currentBlock : int;
	private var blockStock   : int;		// blocos disponiveis no survival
	private var initialized  : bool;
	private var camera       : MW3_Camera;
	private var nextWorldSend : float;
	private var collision    : MW3_CollisionScan;	// v4: colisao do W3 para o MC (mw3_collision.ws)
	private var puppet       : MW3_Puppet;			// v4: Geralt segue o jogador do MC (mw3_puppet.ws)
	private var combat       : MW3_Combat;			// v6: combate W3 <-> MC (mw3_combat.ws)
	private var linkMenu     : bool;				// estado mandado ao MC neste frame (SendLinkState)
	private var linkLoading  : bool;

	default mode        = MW3_Survival;
	default blockStock  = 16;

	private var REACH       : float;	default REACH      = 6.0;
	private var GRID        : float;	default GRID       = 1.0;
	private var MAX_BLOCKS  : int;		default MAX_BLOCKS = 256;

	public function Init()
	{
		if( !initialized )
		{
			// Caminhos confirmados nos bundles do jogo (content0/bundles/blob.bundle).
			blockPaths.PushBack("environment\decorations\decoration_sets\boxes\decoration_set_boxes_crate_b.w2ent");
			blockPaths.PushBack("environment\decorations\containers\crate\crate_cloth_px.w2ent");
			blockPaths.PushBack("environment\decorations\decoration_sets\boxes\decoration_set_boxes_crate_pile_b.w2ent");
			camera = new MW3_Camera in this;
			collision = new MW3_CollisionScan in this;
			puppet = new MW3_Puppet in this;
			initialized = true;
		}
		if( !combat )
			combat = new MW3_Combat in this;

		// Re-registra a cada Initialize: o handler anterior pode ter sido descartado.
		theInput.UnregisterListener( this, 'MW3_ToggleMode' );
		theInput.UnregisterListener( this, 'MW3_PlaceBlock' );
		theInput.UnregisterListener( this, 'MW3_BreakBlock' );
		theInput.UnregisterListener( this, 'MW3_NextBlock' );
		theInput.UnregisterListener( this, 'MW3_CameraMode' );
		theInput.RegisterListener( this, 'OnMW3ToggleMode', 'MW3_ToggleMode' );
		theInput.RegisterListener( this, 'OnMW3PlaceBlock', 'MW3_PlaceBlock' );
		theInput.RegisterListener( this, 'OnMW3BreakBlock', 'MW3_BreakBlock' );
		theInput.RegisterListener( this, 'OnMW3NextBlock',  'MW3_NextBlock' );
		theInput.RegisterListener( this, 'OnMW3CameraMode', 'MW3_CameraMode' );

		ApplyMode();
		CheckNativeBridge();
	}

	public function NotifyCamera( label : string )
	{
		Notify( label );
	}

	public function GetCamera() : MW3_Camera
	{
		return camera;
	}

	public function GetPuppet() : MW3_Puppet
	{
		return puppet;
	}

	// null ate o primeiro CPlayerInput.Initialize.
	public function GetCombat() : MW3_Combat
	{
		return combat;
	}

	// Com o MC linkado, depois de MW3_LinkConnected() e antes da camera: o Geralt vai para onde o
	// jogador do MC esta e a colisao em volta e tracada para o MC. Fora de gameplay, nada.
	public function TickLinked( dt : float )
	{
		if( puppet )
			puppet.Tick( dt, linkMenu, linkLoading );
		if( collision && !linkMenu && !linkLoading )
			collision.Tick();
	}

	// Sem MC: o fantoche solta o Geralt (o jogo volta ao normal).
	public function TickUnlinked()
	{
		if( puppet )
			puppet.Stop();
	}

	// ----------------------------------------------------------- link com o MC

	// Uma vez por frame de gameplay (OnGameCameraPostTick): estado do jogo e do Geralt para o MC.
	// Quando o jogo pausa (menus) este update para; a DLL percebe e devolve o input ao W3.
	public function SendLinkState()
	{
		var pos     : Vector;
		var rot     : EulerAngles;
		var t       : GameTime;
		var hour    : float;
		var menu    : bool;
		var loading : bool;

		if( !thePlayer )
			return;

		// O jogador do MC morreu (kEvPlayerDied, drenado pela DLL): o Geralt morre tambem, pelo caminho
		// normal de morte do W3 (tela de morte + load), como o KillPlayer do SkyCraft (Combat.cpp:400-413).
		// Sem isso o fantoche seguia o respawn do MC ate o vazio (research/07 3.2).
		if( MW3_PollMcDeath() && thePlayer.IsAlive() )
		{
			MW3_Log( "Morte: o jogador do Minecraft morreu; matando o Geralt" );
			if( puppet )
				puppet.Stop();
			thePlayer.Kill( 'MineWitcher3', true );
		}

		if( theGame.GetEngineTimeAsSeconds() >= nextWorldSend )
		{
			MW3_SendWorld( theGame.GetWorld().GetDepotPath() );
			nextWorldSend = theGame.GetEngineTimeAsSeconds() + 1.0;
		}

		t       = theGame.GetGameTime();
		hour    = (float)GameTimeHours( t ) + (float)GameTimeMinutes( t ) / 60.0;
		menu    = theGame.GetGuiManager().IsAnyMenu() || theGame.IsDialogOrCutscenePlaying();
		loading = theGame.IsLoadingScreenVideoPlaying() || theGame.IsBlackscreenOrFading();
		MW3_SendGameState( true, menu, loading, hour );
		linkMenu    = menu;
		linkLoading = loading;

		pos = thePlayer.GetWorldPosition();
		rot = thePlayer.GetWorldRotation();	// campo de struct so pode ser lido de uma variavel
		MW3_SendPlayerState( pos.X, pos.Y, pos.Z, rot.Yaw );
		// O fantoche e a colisao (v4) rodam em TickLinked, chamado pela camera depois disto.
		// v6: atores para o MC, golpes do MC no W3, dano no Geralt para o MC (inerte sem o MC linkado).
		if( combat )
			combat.Tick( menu, loading );
	}

	private function CheckNativeBridge()
	{
		var version : int;

		version = MW3_Ping();
		MW3_Log( "Init no CPlayerInput; native v" + version );
		if( version != MW3_EXPECTED_NATIVE_VERSION() )
			Notify( "MineWitcher3: versao do mw3.asi incompativel (" + version + ")" );
	}

	// ------------------------------------------------------------------ modos

	public function SetMode( newMode : EMW3GameMode )
	{
		mode = newMode;
		ApplyMode();
		if( mode == MW3_Creative )
			Notify("Modo de jogo: CRIATIVO");
		else
			Notify("Modo de jogo: SOBREVIVENCIA (blocos: " + blockStock + ")");
	}

	public function GetMode() : EMW3GameMode
	{
		return mode;
	}

	// Mesmo efeito do cheat oficial god() (game/temp.ws), mas num canal proprio do mod.
	private function ApplyMode()
	{
		if( !thePlayer )
			return;

		if( mode == MW3_Creative )
		{
			thePlayer.SetImmortalityMode( AIM_Invulnerable, AIC_Cheat, true );
			thePlayer.SetCanPlayHitAnim( false );
			thePlayer.AddBuffImmunity_AllNegative( 'mw3_creative', true );
			StaminaBoyInternal( true );
		}
		else
		{
			thePlayer.SetImmortalityMode( AIM_None, AIC_Cheat, true );
			thePlayer.SetCanPlayHitAnim( true );
			thePlayer.RemoveBuffImmunity_AllNegative( 'mw3_creative' );
			StaminaBoyInternal( false );
		}
	}

	// ----------------------------------------------------------------- blocos

	private function TraceFromCamera( out hitPos : Vector, out hitNormal : Vector ) : bool
	{
		var from, to : Vector;

		from = theCamera.GetCameraPosition();
		to   = from + VecNormalize( theCamera.GetCameraDirection() ) * REACH;
		return theGame.GetWorld().StaticTrace( from, to, hitPos, hitNormal );
	}

	private function Snap( v : Vector ) : Vector
	{
		var r : Vector;

		r.X = (float)FloorF( v.X / GRID ) * GRID + GRID * 0.5;
		r.Y = (float)FloorF( v.Y / GRID ) * GRID + GRID * 0.5;
		r.Z = (float)FloorF( v.Z / GRID ) * GRID;
		r.W = 1.0;
		return r;
	}

	public function PlaceBlock()
	{
		var hitPos, hitNormal, pos : Vector;
		var rot      : EulerAngles;
		var template : CEntityTemplate;
		var ent      : CEntity;

		if( mode == MW3_Survival && blockStock <= 0 )
		{
			Notify("Sem blocos! Quebre blocos para coletar.");
			return;
		}
		if( blocks.Size() >= MAX_BLOCKS )
		{
			Notify("Limite de blocos atingido (" + MAX_BLOCKS + ")");
			return;
		}
		if( !TraceFromCamera( hitPos, hitNormal ) )
			return;

		// Empurra meio bloco na direcao da normal para encostar na superficie.
		pos = Snap( hitPos + hitNormal * ( GRID * 0.5 ) );

		template = (CEntityTemplate)LoadResource( blockPaths[ currentBlock ], true );
		if( !template )
		{
			Notify("Template nao encontrado: " + blockPaths[ currentBlock ]);
			return;
		}

		ent = theGame.CreateEntity( template, pos, rot );
		if( ent )
		{
			blocks.PushBack( ent );
			if( mode == MW3_Survival )
				blockStock -= 1;
		}
	}

	public function BreakBlock()
	{
		var hitPos, hitNormal, target : Vector;
		var i, best : int;
		var d, bestDist : float;

		if( TraceFromCamera( hitPos, hitNormal ) )
			target = hitPos;
		else
			target = thePlayer.GetWorldPosition() + thePlayer.GetHeadingVector() * 1.5;

		best = -1;
		bestDist = GRID * 1.5;
		for( i = 0; i < blocks.Size(); i += 1 )
		{
			if( !blocks[i] )
				continue;
			d = VecDistance( blocks[i].GetWorldPosition(), target );
			if( d < bestDist )
			{
				bestDist = d;
				best = i;
			}
		}

		if( best < 0 )
			return;

		blocks[best].Destroy();
		blocks.Erase( best );
		if( mode == MW3_Survival )
			blockStock += 1;
	}

	public function NextBlock()
	{
		currentBlock = ( currentBlock + 1 ) % blockPaths.Size();
		Notify("Bloco selecionado: " + ( currentBlock + 1 ) + "/" + blockPaths.Size());
	}

	private function Notify( msg : string )
	{
		theGame.GetGuiManager().ShowNotification( msg, 2.0 );
		LogChannel( 'MineWitcher3', msg );
	}

	// ------------------------------------------------------------------ input

	event OnMW3ToggleMode( action : SInputAction )
	{
		if( IsPressed( action ) )
		{
			if( mode == MW3_Creative )
				SetMode( MW3_Survival );
			else
				SetMode( MW3_Creative );
		}
	}

	event OnMW3PlaceBlock( action : SInputAction )
	{
		if( IsPressed( action ) )
			PlaceBlock();
	}

	event OnMW3BreakBlock( action : SInputAction )
	{
		if( IsPressed( action ) )
			BreakBlock();
	}

	event OnMW3CameraMode( action : SInputAction )
	{
		if( IsPressed( action ) && camera )
			Notify( camera.Cycle() );
	}

	event OnMW3NextBlock( action : SInputAction )
	{
		if( IsPressed( action ) )
			NextBlock();
	}
}

// Comandos de console (precisa de -debugscripts / console habilitado).
exec function gamemode( m : int )
{
	if( m == 1 )
		MW3_Get().SetMode( MW3_Creative );
	else
		MW3_Get().SetMode( MW3_Survival );
}
