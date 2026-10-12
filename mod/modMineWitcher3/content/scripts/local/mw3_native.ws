// Natives implementadas em mw3.asi (native/src/natives.cpp), registradas via WitcherNativeBridge.
// Sem mw3.asi + WitcherNativeBridge.asi carregados, estes imports fazem a compilacao dos scripts falhar.

import function MW3_Ping() : int;
import function MW3_Log( msg : string );
import function MW3_PollCameraKey() : int;

// ---- link com o Minecraft (native v3) -----------------------------------------------------------
// Chame MW3_LinkConnected() uma vez por frame ANTES dos MW3_Mc*: ela le o estado do Minecraft e
// congela o snapshot que os getters devolvem. Sem Minecraft linkado os getters devolvem o ultimo valor.
import function MW3_LinkConnected() : bool;
import function MW3_InputRouted() : bool;			// teclado/mouse indo para o MC agora (o W3 nao os ve)
import function MW3_McEyeX() : float;				// olho do jogador do MC, espaco W3 (metros, Z para cima)
import function MW3_McEyeY() : float;
import function MW3_McEyeZ() : float;
import function MW3_McYaw() : float;				// olhar (EulerAngles.Yaw do W3; sinal ainda a confirmar)
import function MW3_McPitch() : float;				// olhar (EulerAngles.Pitch do W3; positivo = para cima, a confirmar)
import function MW3_McCameraMode() : int;			// F5 do MC: 0 1a pessoa, 1 3a por tras, 2 3a de frente
import function MW3_McCameraDistance() : float;		// olho -> camera do MC (metros, ja com o zoom colidido)
import function MW3_McFov() : float;				// FOV vertical do MC (graus)

// Script -> MC, uma vez por frame de gameplay: primeiro o estado do jogo, depois o do jogador.
import function MW3_SendGameState( inGame : bool, menuOpen : bool, loading : bool, gameHour : float );
import function MW3_SendWorld( depotPath : string );
import function MW3_SendPlayerState( x : float, y : float, z : float, yaw : float );

// ---- colisao Stage A (native v4, mw3_collision.ws) ---------------------------------------------
// Por frame: MW3_ColBegin; depois, enquanto houver orcamento, MW3_ColNext escolhe uma coluna
// (mais perto do jogador primeiro), o script traca para baixo/para cima e entrega os hits.
import function MW3_ColBegin( x : float, y : float, z : float ) : bool;	// pes do Geralt; false = nao tracar neste frame
import function MW3_ColNext() : int;				// 0 nada; 1 coluna pronta; 2 coluna + nivel da agua
import function MW3_ColX() : float;					// coluna pendente (espaco W3)
import function MW3_ColY() : float;
import function MW3_ColTop() : float;				// tracar de (x, y, top) ate (x, y, bottom) e de volta
import function MW3_ColBottom() : float;
import function MW3_ColHitDown( z : float );		// superficie atingida descendo
import function MW3_ColHitUp( z : float );			// face de baixo atingida subindo
import function MW3_ColCommit( water : float );		// nivel da agua (W3 z) ou < -1000000 sem agua
import function MW3_ColBudget() : int;				// traces por frame (mw3.ini [Collision] iTracesPerFrame)

// ---- fantoche (native v4, mw3_puppet.ws); depois de MW3_LinkConnected() no mesmo frame ---------
import function MW3_PuppetActive() : bool;			// o MC soltou o jogador no ultimo teleporte: ele dirige o Geralt
import function MW3_McFeetX() : float;				// pes do jogador do MC, espaco W3
import function MW3_McFeetY() : float;
import function MW3_McFeetZ() : float;
import function MW3_McVelX() : float;				// velocidade do MC (m/s, espaco W3)
import function MW3_McVelY() : float;
import function MW3_McVelZ() : float;
import function MW3_McFlags() : int;				// 4 no chao, 8 agachado, 16 correndo, 32 morto, 64 nadando, 128 voando
import function MW3_PuppetMoved( x : float, y : float, z : float );	// onde o script pos o Geralt neste frame

// ---- native v5 -------------------------------------------------------------------------------
import function MW3_PollMcDeath() : bool;			// true uma vez por morte do jogador do MC (o Geralt morre junto)
import function MW3_RequestResync();

// ---- native v6: combate (mw3_combat.ws) --------------------------------------------------------
import function MW3_CombatLinked() : bool;			// MC vivo e no mundo; sem isso o combate fica inerte
import function MW3_ActorsBegin();					// tabela de atores para o MC: Begin, Add..., Commit
import function MW3_ActorAdd( id : int, x : float, y : float, z : float, yaw : float, radius : float, height : float, health : float, maxHealth : float, flags : int, level : int, displayName : string );
import function MW3_ActorsCommit() : int;			// escreve a tabela (vazia se o MC nao esta linkado)
import function MW3_ActorsClear();					// tabela vazia: o MC remove todos os proxies
import function MW3_PollHit() : int;				// id do ator do proximo golpe do MC (0 = nenhum); depois MW3_Hit*
import function MW3_HitDamage() : float;			// dano do MC (pontos de vida do MC, ja com crit/encantamentos)
import function MW3_HitFlags() : int;				// 1 critico, 2 projetil, 4 sweep, 8 fogo
import function MW3_HitWeapon() : int;				// 0 mao, 1 espada, 2 machado, 3 contundente, 4 perfurante, 5 flecha
import function MW3_HitPush() : float;				// forca do knockback do MC (0.4 normal, ~0.9 correndo)
import function MW3_HitPushX() : float;				// direcao do knockback, espaco W3
import function MW3_HitPushY() : float;
import function MW3_SendPlayerHurt( kind : int, mcDamage : float, attackerId : int, flags : int ) : bool;	// dano no jogador do MC (pontos de vida do MC)				// fantoche bloqueado: a DLL teleporta o MC para o Geralt

function MW3_EXPECTED_NATIVE_VERSION() : int
{
	return 6;
}
