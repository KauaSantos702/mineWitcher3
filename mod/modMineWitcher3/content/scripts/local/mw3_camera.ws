// =============================================================================
// MineWitcher3 - cameras estilo Minecraft (F5): 1a pessoa, 3a por tras, 3a de frente.
// Com o Minecraft linkado (MW3_LinkConnected) o olho, o yaw/pitch, o modo F5 e a distancia vem
// do MC pela DLL (ApplyLinked). Sem ele, o prototipo local: rotacao do mouse do proprio W3, pivot
// na cabeca do Geralt e o F5 ciclando aqui (Apply).
// =============================================================================

enum EMW3CamMode
{
	MW3CAM_Vanilla,
	MW3CAM_FirstPerson,
	MW3CAM_ThirdBack,
	MW3CAM_ThirdFront
}

// Os estados do player (Exploration, Combat, Swimming...) encadeiam ate esta versao
// via super/parent (ver extendedMovable.ws:142), entao o wrap vale para quase todos.
// O parser so aceita 'function' depois de @wrapMethod, mesmo quando o original e um 'event';
// sem tipo de retorno explicito (eventos sao sempre bool).
@wrapMethod(CR4Player)
function OnGameCameraPostTick( out moveData : SCameraMovementData, dt : float )
{
	var res : bool;
	var cam : MW3_Camera;

	res = wrappedMethod( moveData, dt );
	cam = MW3_Get().GetCamera();	// null ate o primeiro CPlayerInput.Initialize
	if( !cam )
		return res;

	// Estado do jogo e do Geralt para o MC, a cada frame (o MC faz pacing pelo GameState).
	MW3_Get().SendLinkState();

	if( MW3_LinkConnected() )
	{
		// v4: Geralt nos pes do jogador do MC + colisao em volta para o MC (mw3_puppet/mw3_collision).
		MW3_Get().TickLinked( dt );
		// O F5 vai direto para o MC pela DLL (a camera F5 do MC cicla); o modo vem de volta aqui.
		if( cam.ApplyLinked( moveData, dt ) )
			return true;
		return res;
	}
	MW3_Get().TickUnlinked();

	// F5 chega pela DLL: o motor usa F5 fixo para quicksave e a WndProc o engole antes.
	if( MW3_PollCameraKey() > 0 )
		MW3_Get().NotifyCamera( cam.Cycle() );

	if( cam.Apply( moveData, dt ) )
		return true;
	return res;
}

class MW3_Camera
{
	private var mode         : EMW3CamMode;
	private var playerHidden : bool;
	private var thirdDist    : float;	default thirdDist = 4.0;	// mesma distancia do Minecraft
	private var nextRefresh  : float;

	// Camera frontal: o motor guarda pivotRotationValue entre frames e o controller puxa para
	// o heading "desejado". Por isso guardamos a rotacao do personagem (frontBase) e so
	// aplicamos o delta do mouse em relacao ao que gravamos no frame anterior (frontWritten).
	private var frontActive  : bool;
	private var frontBase    : EulerAngles;
	private var frontWritten : EulerAngles;

	// Camera dirigida pelo Minecraft (ApplyLinked).
	private var linkedActive : bool;

	// Suavizacao vertical do olho: o MC sobe degraus de colisao de uma vez (step-up), e o terreno do W3
	// vira degraus de 1/8 de bloco. So o Z e amortecido (constante STEP_TAU); X/Y seguem exatos.
	// Diferencas grandes (pulo longo, teleporte) passam direto.
	private var smoothZ      : float;
	private var smoothValid  : bool;
	private var STEP_TAU     : float;	default STEP_TAU  = 0.09;
	private var STEP_SNAP    : float;	default STEP_SNAP = 1.2;
	private var linkedMode   : int;		default linkedMode = -1;

	// Conversao do FOV vertical do MC para o fov do CCustomCamera (ver W3FovFromMc).
	// fov_W3 = 2*atan( tan(fovV_MC/2) * fovRefAspect ):
	//   4/3  -> o fov do W3 e horizontal medido num quadro 4:3 (Hor+; hipotese adotada)
	//   16/9 -> horizontal no back buffer 16:9 (Vert-)
	//   1.0  -> o fov do W3 ja e vertical (identidade)
	private var fovRefAspect : float;	default fovRefAspect = 1.3333333;

	public function GetMode() : EMW3CamMode
	{
		return mode;
	}

	// Ordem do F5 no Minecraft: 1a pessoa -> por tras -> de frente -> 1a pessoa.
	// MW3CAM_Vanilla fica no ciclo enquanto o Minecraft nao controla o personagem.
	public function Cycle() : string
	{
		switch( mode )
		{
			case MW3CAM_Vanilla:		mode = MW3CAM_FirstPerson;	break;
			case MW3CAM_FirstPerson:	mode = MW3CAM_ThirdBack;	break;
			case MW3CAM_ThirdBack:		mode = MW3CAM_ThirdFront;	break;
			default:					mode = MW3CAM_Vanilla;		break;
		}

		SetPlayerHidden( mode == MW3CAM_FirstPerson );
		MW3_Log( ModeLabel() );
		return ModeLabel();
	}

	public function ModeLabel() : string
	{
		switch( mode )
		{
			case MW3CAM_FirstPerson:	return "Camera: 1a pessoa";
			case MW3CAM_ThirdBack:		return "Camera: 3a pessoa (tras)";
			case MW3CAM_ThirdFront:		return "Camera: 3a pessoa (frente)";
		}
		return "Camera: padrao do Witcher 3";
	}

	// Chamado a cada update da camera de gameplay. Retorna false para manter o vanilla.
	public function Apply( out moveData : SCameraMovementData, dt : float ) : bool
	{
		var eye  : Vector;
		var rot  : EulerAngles;
		var dist : float;

		if( !thePlayer )
			return false;

		// O MC acabou de soltar a camera: volta ao modo local que estava escolhido.
		if( linkedActive )
		{
			linkedActive = false;
			smoothValid  = false;
			linkedMode   = -1;
			SetPlayerHidden( mode == MW3CAM_FirstPerson );
			RestoreFov();
			MW3_Log( "Camera: Minecraft desconectado; " + ModeLabel() );
		}

		if( mode != MW3CAM_ThirdFront && frontActive )
			LeaveFront( moveData );

		if( mode == MW3CAM_Vanilla )
			return false;

		if( theGame.GetEngineTimeAsSeconds() >= nextRefresh )
		{
			Refresh();
			nextRefresh = theGame.GetEngineTimeAsSeconds() + 1.0;
		}

		eye = HeadPosition();
		if( mode == MW3CAM_FirstPerson )
			dist = 0.0;
		else
			dist = thirdDist;

		WritePivot( moveData, eye, dist );

		if( mode == MW3CAM_ThirdFront )
		{
			rot = UpdateFront( moveData.pivotRotationValue );
			WriteRotation( moveData, rot );
		}

		return true;
	}

	// Camera com o Minecraft linkado: olho, olhar, modo F5 e distancia do MC. O mouse nao chega ao
	// W3 (a DLL o manda ao MC e integra o yaw/pitch com a sensibilidade do MC), entao a rotacao e
	// escrita inteira a cada frame. Chamar depois de MW3_LinkConnected() no mesmo frame.
	private function SmoothEyeZ( z : float, dt : float ) : float
	{
		var a : float;

		if( !smoothValid || AbsF( z - smoothZ ) > STEP_SNAP || dt <= 0.0 )
		{
			smoothZ = z;
			smoothValid = true;
			return z;
		}
		a = 1.0 - PowF( 2.718281828, -dt / STEP_TAU );
		smoothZ = smoothZ + ( z - smoothZ ) * a;
		return smoothZ;
	}

	public function ApplyLinked( out moveData : SCameraMovementData, dt : float ) : bool
	{
		var eye    : Vector;
		var rot    : EulerAngles;
		var dist   : float;
		var mcMode : int;

		if( !thePlayer )
			return false;

		if( !linkedActive )
		{
			linkedActive = true;
			linkedMode   = -1;
			frontActive  = false;	// o estado da frontal local nao vale mais
			// O Geralt some em TODOS os modos: em F5 quem aparece e o modelo do jogador do MC,
			// desenhado no overlay (PlayerOverlay no Fabric). O Refresh reaplica a cada 1 s.
			SetPlayerHidden( true );
			MW3_Log( "Camera: Minecraft no controle" );
		}

		mcMode = MW3_McCameraMode();
		if( mcMode != linkedMode )
			linkedMode = mcMode;

		// Todo frame, depois do wrappedMethod: o estado Exploration amortece o fov para 60 antes.
		ApplyMcFov();

		if( theGame.GetEngineTimeAsSeconds() >= nextRefresh )
		{
			Refresh();
			nextRefresh = theGame.GetEngineTimeAsSeconds() + 1.0;
		}

		eye = Vector( MW3_McEyeX(), MW3_McEyeY(), MW3_McEyeZ(), 1.0 );
		eye.Z = SmoothEyeZ( eye.Z, dt );
		dist = 0.0;
		if( mcMode != 0 )
		{
			dist = MW3_McCameraDistance();
			if( dist <= 0.0 )
				dist = thirdDist;
		}

		WritePivot( moveData, eye, dist );

		rot.Yaw   = MW3_McYaw();
		rot.Pitch = ClampF( MW3_McPitch(), -89.0, 89.0 );
		rot.Roll  = 0;
		if( mcMode == 2 )
			rot = Flip( rot );		// F5 duas vezes no MC: camera de frente, olhando para o jogador
		WriteRotation( moveData, rot );

		return true;
	}

	// FOV vertical do MC (graus) -> fov do CCustomCamera. A unica funcao a calibrar: troque
	// fovRefAspect (screenshot do boneco do MC contra um poste proximo; se ele sair maior que o
	// mundo do W3, o fov do W3 esta estreito demais -> aumente fovRefAspect).
	//
	// Por que 4:3 (research/09-avatar-render.md 2.3.2): o script vanilla trata o fov como angulo
	// horizontal (btTaskSummonCreatures.ws:110 usa um cone de 360-(GetFov()+20) em volta do heading
	// da camera), o default e 60 (r4Player.ws:16264, exploration.ws:746) e o W3 e Hor+ no ultrawide,
	// entao o valor nao pode ser horizontal no aspect real. 60 horizontal em 4:3 = 46.8 vertical =
	// 75.2 horizontal em 16:9, o enquadramento do jogo; 60 vertical (91.5 h) ou 60 h em 16:9
	// (36 v) nao batem. Nao verificado no binario: confirmar com screenshot. O back buffer e o
	// overlay do MC sao 16:9 (1920x1080), entao a mesma conta vale para os dois.
	public function W3FovFromMc( mcVerticalDeg : float ) : float
	{
		var halfTan : float;

		halfTan = TanF( Deg2Rad( mcVerticalDeg * 0.5 ) ) * fovRefAspect;
		return 2.0 * Rad2Deg( AtanF( halfTan, 1.0 ) );
	}

	private function ApplyMcFov()
	{
		var camera : CCustomCamera;
		var mcFov  : float;

		mcFov = MW3_McFov();
		if( mcFov < 1.0 || mcFov > 170.0 )
			return;		// ainda sem snapshot do MC
		camera = theGame.GetGameCamera();
		if( camera )
			camera.SetFov( W3FovFromMc( mcFov ) );
	}

	private function RestoreFov()
	{
		var camera : CCustomCamera;

		camera = theGame.GetGameCamera();
		if( camera && thePlayer )
			camera.SetFov( thePlayer.GetExplorationCameraFov() );
	}

	private function WritePivot( out moveData : SCameraMovementData, eye : Vector, dist : float )
	{
		moveData.pivotRotationController.minPitch = -89.0;
		moveData.pivotRotationController.maxPitch =  89.0;

		moveData.pivotPositionValue    = eye;
		moveData.pivotPositionVelocity = Vector( 0, 0, 0 );
		moveData.pivotPositionController.offsetZ = 0.0;
		moveData.pivotPositionController.SetDesiredPosition( eye );

		moveData.pivotDistanceValue    = dist;
		moveData.pivotDistanceVelocity = 0.0;
		moveData.pivotDistanceController.SetDesiredDistance( dist );

		// Sem o deslocamento "por cima do ombro" do W3.
		moveData.cameraLocalSpaceOffset    = Vector( 0, 0, 0 );
		moveData.cameraLocalSpaceOffsetVel = Vector( 0, 0, 0 );
	}

	private function WriteRotation( out moveData : SCameraMovementData, rot : EulerAngles )
	{
		moveData.pivotRotationValue    = rot;
		moveData.pivotRotationVelocity = EulerAngles( 0, 0, 0 );
		moveData.pivotRotationController.SetDesiredHeading( rot.Yaw );
		moveData.pivotRotationController.SetDesiredPitch( rot.Pitch );
	}

	// Mesma formula do Minecraft para a camera frontal: yaw + 180, pitch invertido.
	private function Flip( r : EulerAngles ) : EulerAngles
	{
		var f : EulerAngles;

		f.Yaw   = AngleNormalize( r.Yaw + 180.0 );
		f.Pitch = -r.Pitch;
		f.Roll  = 0;
		return f;
	}

	private function UpdateFront( incoming : EulerAngles ) : EulerAngles
	{
		if( !frontActive )
		{
			frontBase   = incoming;
			frontActive = true;
		}
		else
		{
			// O que mudou desde o nosso ultimo valor e o movimento do mouse.
			frontBase.Yaw   = AngleNormalize( frontBase.Yaw + AngleDistance( incoming.Yaw, frontWritten.Yaw ) );
			frontBase.Pitch = ClampF( frontBase.Pitch + ( incoming.Pitch - frontWritten.Pitch ), -89.0, 89.0 );
		}

		frontWritten = Flip( frontBase );
		return frontWritten;
	}

	// Ao sair da frontal, devolve ao W3 a rotacao "de verdade" do personagem.
	private function LeaveFront( out moveData : SCameraMovementData )
	{
		frontActive = false;
		moveData.pivotRotationValue    = frontBase;
		moveData.pivotRotationVelocity = EulerAngles( 0, 0, 0 );
		moveData.pivotRotationController.SetDesiredHeading( frontBase.Yaw );
		moveData.pivotRotationController.SetDesiredPitch( frontBase.Pitch );
	}

	private function HeadPosition() : Vector
	{
		var idx : int;

		idx = thePlayer.GetHeadBoneIndex();
		if( idx < 0 )
			return thePlayer.GetWorldPosition() + Vector( 0, 0, 1.62 );	// altura do olho no MC
		return MatrixGetTranslation( thePlayer.GetBoneWorldMatrixByIndex( idx ) );
	}

	// Em 1a pessoa (e sempre com o MC linkado) o corpo e as armas montadas somem.
	public function SetPlayerHidden( hide : bool )
	{
		var inv   : CInventoryComponent;
		var items : array<SItemUniqueId>;
		var ent   : CItemEntity;
		var i     : int;

		if( !thePlayer )
			return;

		thePlayer.SetHideInGame( hide );
		inv = thePlayer.GetInventory();
		if( inv )
		{
			inv.GetAllItems( items );
			for( i = 0; i < items.Size(); i += 1 )
			{
				if( !inv.IsItemMounted( items[i] ) && !inv.IsItemHeld( items[i] ) )
					continue;
				ent = inv.GetItemEntityUnsafe( items[i] );
				if( ent )
					ent.SetHideInGame( hide );
			}
		}
		playerHidden = hide;
	}

	// Equipar/sacar a espada cria entidades novas; reaplica o hide periodicamente.
	public function Refresh()
	{
		if( playerHidden )
			SetPlayerHidden( true );
	}
}
