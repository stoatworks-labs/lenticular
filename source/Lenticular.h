#pragma once

#include <FFGLSDK.h>

//AFTER the SDK: this header names FFUInt32 and does not pull the SDK in
//itself, so an include placed above it fails with "unknown type name" errors
//that point at the About block rather than at the include order.
#include "StoatworksAboutParams.h"
#include "Lens.h"

#include <string>

/**
    Lenticular -- a printed lenticular sheet that shows one layer or the
    other, as an FFGL **mixer** for Resolume.

    A lenticular print is two pictures cut into thin strips and interleaved,
    with a sheet of cylindrical lenses on top, one lens per strip pair. Each
    lens focuses the eye onto one point of the print under it, and which
    strip that point is on depends on the angle the card is seen at. Tilt it
    and it flips. This models the lens and the print, and the flip, the
    ghost, the moire of a pitch mismatch, the sweep of a near viewer and the
    ridged, stepped look all fall out of it. See Lens.h and AGENTS.md.

    **This is the fleet's fourth FF_MIXER.** Read the mixer section of
    genlock's AGENTS.md before changing `ProcessOpenGL`: what is written
    there was measured, and several things the SDK's headers imply are not
    true.

    It is stateless: every frame is a pure function of the two inputs and
    the parameters. There is no clock, no buffer and nothing carried from
    one frame to the next, so there is nothing a resize could clear.
*/
class Lenticular : public CFFGLPlugin
{
public:
	Lenticular();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// Neither changes a pixel. They exist so that ONE session in front of
	/// Resolume leaves a log that says which host and which bundle.
	void SetHostInfo( const char* hostname, const char* version ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Optional, not required -- see genlock's AGENTS.md. A four-character
	/// label for a host with no room for "SW Lenticular".
	const char* GetShortName() override
	{
		return "Lent";
	}

	/// What the last rendered frame actually used, for the harness. A check
	/// that disagrees with the picture can then say WHICH side is wrong.
	const lenticular::lens::Setup& SetupForTest() const
	{
		return lastSetup;
	}

	/// The negative controls: a bitmask of lenticular::Fault, 0 in the
	/// shipped plugin, reachable only from here. Each one builds the wrong
	/// answer to a check into the real ProcessOpenGL or the real shader, so
	/// the check can be shown to reject the plugin's own wrong version.
	void SetFaultForTest( int mask )
	{
		fault = mask;
	}

	/// The mutation test: compile this fragment shader instead of the
	/// shipped one. Call before InitGL. Null restores the shipped text.
	void SetFragmentShaderForTest( const char* text )
	{
		fragmentOverride = text;
	}

	/// In the order the host shows them.
	///
	/// **Index 0 is sacrificial.** Resolume Arena 7.27.1 does not expose a
	/// mixer's FIRST parameter at all -- measured on genlock (Key Source),
	/// wipe (Aspect Comp) and relay (Standard): it is absent from Arena's
	/// mixer panel and REST JSON, so it is stuck at its default. Whatever
	/// sits here must therefore be a control whose default is right if
	/// nobody can ever reach it: Squeeze, on, which is what a real interleave
	/// does. Arena, not the harness, must confirm which parameter it hides.
	enum ParamID : FFUInt32
	{
		//Print
		PT_SQUEEZE,
		PT_INTERLEAVE_PITCH,
		PT_BLEED,

		//Lens
		PT_LENS_PITCH,
		PT_FOCAL_LENGTH,
		PT_FOCUS_SPOT,
		PT_DISTANCE,

		//View
		PT_OPACITY,///< the tilt: Arena binds it to the LAYER's opacity fader
		PT_ANGLE_RANGE,
		PT_RIDGE_SHINE,
		PT_LIGHT_ANGLE,

		//About. Last in the enum so nothing before it ever moves.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	ffglex::FFGLShader shader;
	ffglex::FFGLScreenQuad quad;

	lenticular::lens::Setup lastSetup;

	float params[ PT_COUNT ] = {};

	int fault                    = 0;
	const char* fragmentOverride = nullptr;

	//The host-session log: what reached the mixer. Nothing renders from it.
	unsigned long frames   = 0;
	int opacityLines       = 0;
	float loggedOpacity    = -1.0f;
	unsigned guardsLogged  = 0;
	void LogGuard( unsigned bit, const char* what );

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
