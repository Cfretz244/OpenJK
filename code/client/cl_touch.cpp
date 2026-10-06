/*
===========================================================================
On-screen touch controls (mobile).

Fed raw finger events by the SDL input layer (CL_TouchEvent); emits the
same engine events a gamepad would — SE_JOYSTICK_AXIS for the virtual move
stick, SE_MOUSE for drag-look, SE_KEY for buttons — so bindings still apply.
Active only in-game with no menu/console up; CL_TouchOverlayActive() tells
the input layer when to drop SDL's touch-as-mouse synthesis in favour of us.

Layout is in the 2D canvas space (480 tall; x spans the aspect-corrected
width, see RB_SetGL2D). Buttons anchor to the right or left screen edge.
===========================================================================
*/

#include "client.h"
#include "sys/sys_local.h"
#include <math.h>

// __IPHONEOS__ is SDL's define and this file doesn't include SDL.h, so
// detect iOS via Apple's TargetConditionals instead.
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__ANDROID__) || ( defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE )
#define TOUCH_DEFAULT "1"
#else
#define TOUCH_DEFAULT "0"
#endif

static cvar_t *in_touchControls   = NULL;	// 0 off, 1 on
static cvar_t *in_touchLookScale  = NULL;	// multiplier on drag-look pixels
static cvar_t *in_touchSwingTapMs = NULL;	// tap on the look area = one attack
static cvar_t *in_touchAlpha      = NULL;	// overlay opacity

#define TOUCH_MAX_FINGERS	8
#define STICK_RADIUS		70.0f		// canvas units, full deflection
#define STICK_KNOB			30.0f
#define STICK_DEADZONE		0.10f
#define STICK_ZONE_FRAC		0.45f		// left portion of the screen that spawns the stick
#define MENU_LONGPRESS_MS	600			// hold MENU -> console (for tuning cvars without a pad)
#define HIT_SLOP			1.3f		// touch target = drawn radius * this

typedef enum { TB_HOLD, TB_TAP, TB_MENU } touchButtonKind_t;

typedef struct {
	const char	*label;
	float		cx, cy, r;	// cx < 0: from right edge; cx > 0: from left edge
	int			key;
	touchButtonKind_t kind;
} touchButton_t;

// Tuned for saber play: ATTACK big under the right thumb, jump/force around it.
static const touchButton_t touchButtons[] = {
	{ "ATK",  -80,  392, 40, A_MOUSE1, TB_HOLD },
	{ "JMP",  -168, 442, 28, A_SPACE,  TB_HOLD },
	{ "DUCK", -242, 448, 24, A_LOW_C,  TB_HOLD },
	{ "PUSH", -172, 362, 26, A_F1,     TB_TAP  },
	{ "PULL", -116, 300, 26, A_F2,     TB_TAP  },
	{ "USE",  -40,  300, 24, A_LOW_E,  TB_HOLD },
	{ "STYL", -242, 380, 22, A_LOW_L,  TB_TAP  },
	{ "WPN",  -84,  28,  20, A_LOW_R,  TB_TAP  },
	{ "MENU", -32,  28,  20, A_ESCAPE, TB_MENU },
	{ "SAVE",  32,  28,  20, A_F12,    TB_TAP  },
	{ "LOAD",  84,  28,  20, A_F9,     TB_TAP  },
};
#define NUM_TOUCH_BUTTONS ( (int)( sizeof( touchButtons ) / sizeof( touchButtons[0] ) ) )

typedef enum { TF_NONE, TF_STICK, TF_LOOK, TF_BUTTON } touchRole_t;

typedef struct {
	qboolean	active;
	long long	id;
	touchRole_t	role;
	int			button;			// TF_BUTTON
	float		ox, oy;			// origin (canvas): stick centre / press point
	float		x, y;			// current (canvas)
	float		nx, ny;			// current (normalised), for look deltas
	int			startTime;
	float		maxMove;		// canvas units
	qboolean	fired;			// TB_MENU long-press consumed
} touchFinger_t;

static touchFinger_t fingers[TOUCH_MAX_FINGERS];
static int   stickAxis[2] = { 0, 0 };		// last emitted AXIS_SIDE / AXIS_FORWARD
static float lookRemainder[2] = { 0, 0 };
static qboolean wasActive = qfalse;

static void Touch_InitCvars( void ) {
	if ( in_touchControls )
		return;
	in_touchControls   = Cvar_Get( "in_touchControls",   TOUCH_DEFAULT, CVAR_ARCHIVE_ND );
	in_touchLookScale  = Cvar_Get( "in_touchLookScale",  "1.0", CVAR_ARCHIVE_ND );
	in_touchSwingTapMs = Cvar_Get( "in_touchSwingTapMs", "200", CVAR_ARCHIVE_ND );
	in_touchAlpha      = Cvar_Get( "in_touchAlpha",      "0.45", CVAR_ARCHIVE_ND );
}

// Canvas x extents (RB_SetGL2D widens 0..640 symmetrically under r_aspectCorrect2D).
static void Touch_Canvas( float *left, float *right ) {
	*left = 0.0f; *right = 640.0f;
	if ( Cvar_VariableIntegerValue( "r_aspectCorrect2D" ) ) {
		int w43 = cls.glconfig.vidHeight * 4 / 3;
		if ( w43 < cls.glconfig.vidWidth ) {
			float extra = 320.0f * ( (float)cls.glconfig.vidWidth / w43 - 1.0f );
			*left = -extra; *right = 640.0f + extra;
		}
	}
}

static void Touch_ButtonCentre( const touchButton_t *b, float L, float R, float *cx, float *cy ) {
	*cx = ( b->cx < 0 ) ? R + b->cx : L + b->cx;
	*cy = b->cy;
}

qboolean CL_TouchOverlayActive( void ) {
	Touch_InitCvars();
	if ( !in_touchControls->integer )
		return qfalse;
	if ( cls.state != CA_ACTIVE )
		return qfalse;
	if ( Key_GetCatcher() & ( KEYCATCH_UI | KEYCATCH_CONSOLE ) )
		return qfalse;
	if ( CL_IsRunningInGameCinematic() || CL_InGameCinematicOnStandBy() )
		return qfalse;
	return qtrue;
}

static void Touch_Key( int key, qboolean down ) {
	Sys_QueEvent( 0, SE_KEY, key, down, 0, NULL );
}

static void Touch_EmitStick( int side, int forward ) {
	if ( side != stickAxis[0] ) { Sys_QueEvent( 0, SE_JOYSTICK_AXIS, AXIS_SIDE, side, 0, NULL ); stickAxis[0] = side; }
	if ( forward != stickAxis[1] ) { Sys_QueEvent( 0, SE_JOYSTICK_AXIS, AXIS_FORWARD, forward, 0, NULL ); stickAxis[1] = forward; }
}

static void Touch_ReleaseFinger( touchFinger_t *f, qboolean emitTap ) {
	switch ( f->role ) {
	case TF_STICK:
		Touch_EmitStick( 0, 0 );
		break;
	case TF_LOOK:
		// A quick, still tap on the look area is one attack (saber swing).
		if ( emitTap && f->maxMove < 12.0f
			&& cls.realtime - f->startTime <= in_touchSwingTapMs->integer ) {
			Touch_Key( A_MOUSE1, qtrue );
			Touch_Key( A_MOUSE1, qfalse );
		}
		break;
	case TF_BUTTON: {
		const touchButton_t *b = &touchButtons[f->button];
		if ( b->kind == TB_HOLD )
			Touch_Key( b->key, qfalse );
		else if ( b->kind == TB_MENU && emitTap && !f->fired ) {
			Touch_Key( b->key, qtrue );
			Touch_Key( b->key, qfalse );
		}
		break;
	}
	default: break;
	}
	memset( f, 0, sizeof( *f ) );
}

static void Touch_ReleaseAll( void ) {
	for ( int i = 0; i < TOUCH_MAX_FINGERS; i++ )
		if ( fingers[i].active )
			Touch_ReleaseFinger( &fingers[i], qfalse );
	Touch_EmitStick( 0, 0 );
}

static touchFinger_t *Touch_Find( long long id ) {
	for ( int i = 0; i < TOUCH_MAX_FINGERS; i++ )
		if ( fingers[i].active && fingers[i].id == id )
			return &fingers[i];
	return NULL;
}

static int Touch_HitButton( float x, float y, float L, float R ) {
	for ( int i = 0; i < NUM_TOUCH_BUTTONS; i++ ) {
		float cx, cy;
		Touch_ButtonCentre( &touchButtons[i], L, R, &cx, &cy );
		float r = touchButtons[i].r * HIT_SLOP;
		float dx = x - cx, dy = y - cy;
		if ( dx * dx + dy * dy <= r * r )
			return i;
	}
	return -1;
}

static void Touch_UpdateStick( touchFinger_t *f ) {
	float dx = ( f->x - f->ox ) / STICK_RADIUS;
	float dy = ( f->y - f->oy ) / STICK_RADIUS;
	float len = sqrtf( dx * dx + dy * dy );
	if ( len > 1.0f ) { dx /= len; dy /= len; len = 1.0f; }
	if ( len < STICK_DEADZONE ) { Touch_EmitStick( 0, 0 ); return; }
	// rescale so the deadzone edge is 0 and the rim is full speed
	float k = ( len - STICK_DEADZONE ) / ( 1.0f - STICK_DEADZONE ) / len;
	Touch_EmitStick( (int)( dx * k * 127.0f ), (int)( -dy * k * 127.0f ) );
}

/*
type: 0 down, 1 motion, 2 up. nx/ny normalised window coords.
Always called by the input layer; we decide whether to act.
*/
void CL_TouchEvent( long long id, int type, float nx, float ny ) {
	Touch_InitCvars();
	float L, R;
	Touch_Canvas( &L, &R );
	float x = L + nx * ( R - L );
	float y = ny * 480.0f;

	touchFinger_t *f = Touch_Find( id );

	if ( type == 0 ) {
		if ( !CL_TouchOverlayActive() )
			return;
		if ( f )
			Touch_ReleaseFinger( f, qfalse );	// lost the up; start over
		for ( int i = 0; i < TOUCH_MAX_FINGERS; i++ )
			if ( !fingers[i].active ) { f = &fingers[i]; break; }
		if ( !f )
			return;
		memset( f, 0, sizeof( *f ) );
		f->active = qtrue; f->id = id;
		f->ox = f->x = x; f->oy = f->y = y; f->nx = nx; f->ny = ny;
		f->startTime = cls.realtime;

		int b = Touch_HitButton( x, y, L, R );
		if ( b >= 0 ) {
			f->role = TF_BUTTON; f->button = b;
			const touchButton_t *btn = &touchButtons[b];
			if ( btn->kind == TB_HOLD )
				Touch_Key( btn->key, qtrue );
			else if ( btn->kind == TB_TAP ) {
				Touch_Key( btn->key, qtrue );
				Touch_Key( btn->key, qfalse );
			}
			return;
		}
		qboolean stickBusy = qfalse;
		for ( int i = 0; i < TOUCH_MAX_FINGERS; i++ )
			if ( fingers[i].active && fingers[i].role == TF_STICK ) stickBusy = qtrue;
		if ( !stickBusy && x < L + STICK_ZONE_FRAC * ( R - L ) && y > 70.0f ) {
			f->role = TF_STICK;
			return;
		}
		f->role = TF_LOOK;
		return;
	}

	if ( !f )
		return;

	if ( type == 1 ) {
		float ddx = x - f->ox, ddy = y - f->oy;
		float d = sqrtf( ddx * ddx + ddy * ddy );
		if ( d > f->maxMove ) f->maxMove = d;

		if ( f->role == TF_LOOK && CL_TouchOverlayActive() ) {
			float sx = ( nx - f->nx ) * cls.glconfig.vidWidth  * in_touchLookScale->value + lookRemainder[0];
			float sy = ( ny - f->ny ) * cls.glconfig.vidHeight * in_touchLookScale->value + lookRemainder[1];
			int ix = (int)sx, iy = (int)sy;
			lookRemainder[0] = sx - ix; lookRemainder[1] = sy - iy;
			if ( ix || iy )
				Sys_QueEvent( 0, SE_MOUSE, ix, iy, 0, NULL );
		}
		f->x = x; f->y = y; f->nx = nx; f->ny = ny;
		if ( f->role == TF_STICK )
			Touch_UpdateStick( f );
		return;
	}

	// up
	Touch_ReleaseFinger( f, CL_TouchOverlayActive() );
}

// Per frame: release everything the moment the overlay stops applying
// (menu opened, cutscene, level change) so no +action or axis sticks.
void CL_TouchFrame( void ) {
	Touch_InitCvars();
	qboolean active = CL_TouchOverlayActive();
	if ( !active ) {
		if ( wasActive )
			Touch_ReleaseAll();
		wasActive = qfalse;
		return;
	}
	wasActive = qtrue;
	for ( int i = 0; i < TOUCH_MAX_FINGERS; i++ ) {
		touchFinger_t *f = &fingers[i];
		if ( f->active && f->role == TF_BUTTON && touchButtons[f->button].kind == TB_MENU
			&& !f->fired && cls.realtime - f->startTime >= MENU_LONGPRESS_MS ) {
			f->fired = qtrue;
			Touch_Key( A_CONSOLE, qtrue );
			Touch_Key( A_CONSOLE, qfalse );
		}
	}
}

// ---- drawing -------------------------------------------------------------

static void Touch_DrawChar( float x, float y, float w, float h, int ch ) {
	ch &= 255;
	if ( ch == ' ' ) return;
	int row = ch >> 4, col = ch & 15;
	float frow = row * 0.0625f, fcol = col * 0.0625f;
	re.DrawStretchPic( x, y, w, h, fcol, frow, fcol + 0.03125f, frow + 0.0625f, cls.charSetShader );
}

static void Touch_DrawLabel( float cx, float cy, float r, const char *s, const float *color ) {
	int n = (int)strlen( s );
	float h = r * 0.62f;
	float w = h * 0.75f;
	if ( w * n > r * 1.7f ) { w = r * 1.7f / n; h = w / 0.75f; }
	float x = cx - w * n * 0.5f;
	re.SetColor( color );
	for ( int i = 0; i < n; i++ )
		Touch_DrawChar( x + i * w, cy - h * 0.5f, w, h, s[i] );
	re.SetColor( NULL );
}

static void Touch_DrawRing( qhandle_t sh, float cx, float cy, float r, const float *color ) {
	re.SetColor( color );
	re.DrawStretchPic( cx - r, cy - r, 2 * r, 2 * r, 0, 0, 1, 1, sh );
	re.SetColor( NULL );
}

void CL_TouchDraw( void ) {
	if ( !CL_TouchOverlayActive() )
		return;
	qhandle_t ring = re.RegisterShaderNoMip( "gfx/touch/ring" );
	qhandle_t disc = re.RegisterShaderNoMip( "gfx/touch/disc" );
	if ( !ring ) ring = cls.whiteShader;
	if ( !disc ) disc = cls.whiteShader;

	float L, R;
	Touch_Canvas( &L, &R );
	float a = in_touchAlpha->value;
	vec4_t idle    = { 0.75f, 0.85f, 1.0f, a };
	vec4_t pressed = { 1.0f, 1.0f, 1.0f, a + 0.35f };
	vec4_t text    = { 1.0f, 1.0f, 1.0f, a + 0.25f };

	for ( int i = 0; i < NUM_TOUCH_BUTTONS; i++ ) {
		const touchButton_t *b = &touchButtons[i];
		float cx, cy;
		Touch_ButtonCentre( b, L, R, &cx, &cy );
		qboolean down = qfalse;
		for ( int j = 0; j < TOUCH_MAX_FINGERS; j++ )
			if ( fingers[j].active && fingers[j].role == TF_BUTTON && fingers[j].button == i ) down = qtrue;
		Touch_DrawRing( ring, cx, cy, b->r, down ? pressed : idle );
		Touch_DrawLabel( cx, cy, b->r, b->label, text );
	}

	for ( int j = 0; j < TOUCH_MAX_FINGERS; j++ ) {
		touchFinger_t *f = &fingers[j];
		if ( !f->active || f->role != TF_STICK ) continue;
		float dx = f->x - f->ox, dy = f->y - f->oy;
		float len = sqrtf( dx * dx + dy * dy );
		if ( len > STICK_RADIUS ) { dx *= STICK_RADIUS / len; dy *= STICK_RADIUS / len; }
		Touch_DrawRing( ring, f->ox, f->oy, STICK_RADIUS, idle );
		Touch_DrawRing( disc, f->ox + dx, f->oy + dy, STICK_KNOB, pressed );
	}
}
