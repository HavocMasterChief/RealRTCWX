/*
===========================================================================
uwp_display.cpp -- TV refresh rate for the Xbox build (RealRTCWX)

Performance mode asks the TV for 120 Hz, Quality for 60 Hz, at the current
resolution. If the TV (or the console's video settings) doesn't offer the
rate, nothing changes. The request is asynchronous; the console switches
modes on its own and the game keeps running.

This file is part of RealRTCW and is licensed under the GPL v3.
===========================================================================
*/

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Display.Core.h>

#include <cmath>
#include <cstdio>

using namespace winrt::Windows::Graphics::Display::Core;

extern "C" void Com_Printf( const char *fmt, ... );

static int uwp_requestedHz;

extern "C" void UWP_RequestRefreshRate( int hz ) {
	if ( hz == uwp_requestedHz ) {
		return;
	}
	uwp_requestedHz = hz;

	try {
		HdmiDisplayInformation info = HdmiDisplayInformation::GetForCurrentView();
		if ( !info ) {
			Com_Printf( "display: no HDMI display information\n" );
			return;
		}
		HdmiDisplayMode current = info.GetCurrentDisplayMode();
		if ( std::fabs( current.RefreshRate() - hz ) < 1.5 ) {
			Com_Printf( "display: already at %.2f Hz\n", current.RefreshRate() );
			return;
		}

		HdmiDisplayMode best{ nullptr };
		for ( HdmiDisplayMode const &m : info.GetSupportedDisplayModes() ) {
			if ( m.ResolutionWidthInRawPixels() != current.ResolutionWidthInRawPixels() ||
				 m.ResolutionHeightInRawPixels() != current.ResolutionHeightInRawPixels() ||
				 m.ColorSpace() != current.ColorSpace() ||
				 m.BitsPerPixel() != current.BitsPerPixel() ||
				 m.StereoEnabled() ) {
				continue;
			}
			if ( std::fabs( m.RefreshRate() - hz ) < 1.5 ) {
				best = m;
				break;
			}
		}
		if ( !best ) {
			Com_Printf( "display: %d Hz not offered at %ux%u (staying at %.2f Hz)\n", hz,
						current.ResolutionWidthInRawPixels(), current.ResolutionHeightInRawPixels(),
						current.RefreshRate() );
			return;
		}
		Com_Printf( "display: requesting %.2f Hz\n", best.RefreshRate() );
		info.RequestSetCurrentDisplayModeAsync( best );   // fire and forget
	} catch ( winrt::hresult_error const &e ) {
		Com_Printf( "display: refresh rate change failed (0x%08X)\n", (unsigned)e.code() );
	} catch ( ... ) {
		Com_Printf( "display: refresh rate change failed\n" );
	}
}
