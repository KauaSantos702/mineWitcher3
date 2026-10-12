// =============================================================================
// MineWitcher3 - colisao do W3 para o Minecraft, Stage A (research/04 secao 2).
// Grade de raios verticais em volta do jogador: a DLL escolhe as colunas (mais perto primeiro, para
// o holdUntilReady do MC soltar logo) e monta os triangulos/voxels; aqui so os StaticTrace.
// Sem o Minecraft linkado MW3_ColBegin devolve false e nada e tracado.
// =============================================================================

class MW3_CollisionScan
{
	private var groups : array<name>;
	private var ready  : bool;

	// Superficies empilhadas por coluna, em cada sentido (pontes, telhados, cavernas).
	private var MAX_HITS : int;		default MAX_HITS = 6;
	// Recomeco quando o raio nasce dentro de um solido convexo (o PhysX acusa hit na origem).
	private var SKIP     : float;	default SKIP     = 0.25;
	private var diagDone : bool;

	private function Setup()
	{
		// Grupos que colidem com o Character na collision_matrix.csv (research/04 2.2). Water a parte.
		groups.Clear();
		groups.PushBack( 'Terrain' );
		groups.PushBack( 'Static' );
		groups.PushBack( 'Foliage' );
		groups.PushBack( 'Fence' );
		groups.PushBack( 'Door' );
		groups.PushBack( 'Platforms' );
		groups.PushBack( 'Boat' );
		groups.PushBack( 'BoatDocking' );
		groups.PushBack( 'DeniedArea' );
		ready = true;
	}

	// Loga, para a coluna do jogador, o primeiro hit descendo de pes+24 e de pes+2 com cada grupo,
	// com todos e sem lista (default do motor). Serve para achar chao "errado" (research: 8 m abaixo).
	private function Diagnose( world : CWorld, feet : Vector )
	{
		var one : array<name>;
		var i   : int;

		MW3_Log( "colisao diag: pes do Geralt z=" + FloatToString( feet.Z ) + " (x=" + FloatToString( feet.X ) + " y=" + FloatToString( feet.Y ) + ")" );
		DiagLine( world, feet, groups, "todos" );
		one.Clear();
		DiagLine( world, feet, one, "sem lista" );
		for( i = 0; i < groups.Size(); i += 1 )
		{
			one.Clear();
			one.PushBack( groups[i] );
			DiagLine( world, feet, one, NameToString( groups[i] ) );
		}
	}

	private function DiagLine( world : CWorld, feet : Vector, g : array<name>, label : string )
	{
		var hitPos, hitNormal : Vector;
		var s : string;

		s = "colisao diag [" + label + "] de +24: ";
		if( world.StaticTrace( Vector( feet.X, feet.Y, feet.Z + 24.0, 1.0 ), Vector( feet.X, feet.Y, feet.Z - 32.0, 1.0 ), hitPos, hitNormal, g ) )
			s = s + FloatToString( hitPos.Z );
		else
			s = s + "nada";
		s = s + " | de +2: ";
		if( world.StaticTrace( Vector( feet.X, feet.Y, feet.Z + 2.0, 1.0 ), Vector( feet.X, feet.Y, feet.Z - 32.0, 1.0 ), hitPos, hitNormal, g ) )
			s = s + FloatToString( hitPos.Z );
		else
			s = s + "nada";
		MW3_Log( s );
	}

	// Uma vez por frame de gameplay, com o MC linkado (OnGameCameraPostTick).
	public function Tick()
	{
		var world     : CWorld;
		var feet      : Vector;
		var hitPos    : Vector;
		var hitNormal : Vector;
		var budget    : int;
		var kind      : int;
		var hits      : int;
		var x, y      : float;
		var top, bottom, z : float;
		var ground    : float;
		var water     : float;
		var depth     : float;
		var haveGround : bool;

		if( !thePlayer )
			return;
		feet = thePlayer.GetWorldPosition();
		if( !MW3_ColBegin( feet.X, feet.Y, feet.Z ) )
			return;
		if( !ready )
			Setup();

		world  = theGame.GetWorld();
		if( !diagDone )
		{
			diagDone = true;
			Diagnose( world, feet );
		}
		budget = MW3_ColBudget();
		// O orcamento so e conferido entre colunas: uma coluna comecada sempre vai inteira.
		while( budget > 0 )
		{
			kind = MW3_ColNext();
			if( kind == 0 )
				break;
			x      = MW3_ColX();
			y      = MW3_ColY();
			top    = MW3_ColTop();
			bottom = MW3_ColBottom();

			// Descendo: topo de cada superficie.
			z = top;
			hits = 0;
			haveGround = false;
			while( hits < MAX_HITS && z > bottom )
			{
				hits += 1;
				if( !world.StaticTrace( Vector( x, y, z, 1.0 ), Vector( x, y, bottom, 1.0 ), hitPos, hitNormal, groups ) )
					break;
				if( hitPos.Z >= z - 0.01 )
				{
					z -= SKIP;
					continue;
				}
				MW3_ColHitDown( hitPos.Z );
				if( !haveGround )
				{
					ground = hitPos.Z;
					haveGround = true;
				}
				z = hitPos.Z - 0.05;
			}
			budget -= hits;

			// Subindo: faces de baixo (tetos, pontes, beirais).
			z = bottom;
			hits = 0;
			while( hits < MAX_HITS && z < top )
			{
				hits += 1;
				if( !world.StaticTrace( Vector( x, y, z, 1.0 ), Vector( x, y, top, 1.0 ), hitPos, hitNormal, groups ) )
					break;
				if( hitPos.Z <= z + 0.01 )
				{
					z += SKIP;
					continue;
				}
				MW3_ColHitUp( hitPos.Z );
				z = hitPos.Z + 0.05;
			}
			budget -= hits;

			// Agua: uma consulta por bloco (a DLL pede em 1 de cada 4 colunas), no chao da coluna.
			water = -10000000.0;	// sem agua
			if( kind == 2 )
			{
				if( !haveGround )
					ground = bottom;
				depth = world.GetWaterDepth( Vector( x, y, ground, 1.0 ) );
				if( depth < 1000.0 )
					water = world.GetWaterLevel( Vector( x, y, ground, 1.0 ) );
			}
			MW3_ColCommit( water );
		}
	}
}
