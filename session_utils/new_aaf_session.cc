/*
 * Copyright (C) 2023 Adrien Gesta-Fline <dev.agfline@posteo.net>
 * Based on Robin Gareus <robin@gareus.org> session_utils files
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */


#include <unistd.h>
#include <cstdlib>
#include <getopt.h>
#include <iostream>
#include <glibmm.h>

#include <libxml/tree.h>
#include <libxml/parser.h>
#include <libxml/xpath.h>
#include <libxml/xpathInternals.h>

#include "ardour/audioengine.h"
#include "ardour/filename_extensions.h"
#include "ardour/template_utils.h"
#include "ardour/session_directory.h"

#include "ardour/import_status.h"
#include "ardour/region_factory.h"
#include "ardour/playlist.h"
#include "ardour/source_factory.h"
#include "ardour/audioregion.h"
#include "ardour/audiofilesource.h"
#include "ardour/audio_track.h"

#include "pbd/file_utils.h"
#include "common.h"

#include "aaf/libaaf.h"
#include "aaf/utils.h"


using namespace std;
using namespace ARDOUR;
using namespace SessionUtils;
using namespace Timecode;
using namespace PBD;


#ifndef UTILNAME
#define UTILNAME "ardour_aafimport"
#endif


/*
 *  TODO:
 *    - Track level
 *    - Track pan
 *    - Track level automation
 *    - Track pan automation
 *    - Region level automation ?
 *    - Session timecode offset (so the very begining of the timeline starts at eg. 01:00:00:00)
 *    - Markers
 *    x Multichannel audio file import (AAFOperationDef_AudioChannelCombiner)
 *    - Multichannel region from multiple source audio files (1 file per channel) ?
 *    - Mono region from a specific channel of a multichannel file ?
 *    - Muted region
 */


#define PRINT_I( fmt, ... ) \
    { fprintf( stderr, "[\033[1;38;5;81mi\x1B[0m] \x1b[38;5;239m%s : %s() on line %i :\x1B[0m ", __FILE__, __func__, __LINE__ ); fprintf( stderr, fmt, ##__VA_ARGS__ ); }
#define PRINT_W( fmt, ... ) \
    { fprintf( stderr, "[\x1B[33mw\x1B[0m] \x1b[38;5;239m%s : %s() on line %i :\x1B[0m ", __FILE__, __func__, __LINE__ ); fprintf( stderr, fmt, ##__VA_ARGS__ ); }
#define PRINT_E( fmt, ... ) \
    { fprintf( stderr, "[\x1B[31me\x1B[0m] \x1b[38;5;239m%s : %s() on line %i :\x1B[0m ", __FILE__, __func__, __LINE__ ); fprintf( stderr, fmt, ##__VA_ARGS__ ); }





static void usage();
static void list_templates();
static std::string template_path_from_name( std::string const& name );
static Session* create_new_session( string const &dir, string const &state, float samplerate, ARDOUR::SampleFormat bitdepth, int master_bus_chn, string const &template_path );
// static void set_session_video_from_aaf( Session *s, AAF_Iface *aafi );
static std::shared_ptr<AudioTrack> get_nth_audio_track( uint32_t nth, std::shared_ptr<RouteList const> routes );
static bool import_sndfile_as_region( Session *s, struct aafiAudioEssence *audioEssence, SrcQuality quality, timepos_t &pos, SourceList &sources, ImportStatus &status, vector<std::shared_ptr<Region> > *regions /* boost::shared_ptr<Region> r*/ );
static void set_session_range( Session *s, AAF_Iface *aafi );
static std::shared_ptr<Region> create_region( vector<std::shared_ptr<Region> > source_regions, aafiAudioClip *aafAudioClip, SourceList& oneClipSources, aafPosition_t clipOffset, aafRational_t samplerate_r );
static void set_region_gain( aafiAudioClip *aafAudioClip, std::shared_ptr<Region> region );
static std::shared_ptr<AudioTrack> prepare_audio_track( aafiAudioTrack *aafTrack, Session *s );
static void set_region_fade( aafiAudioClip *aafAudioClip, std::shared_ptr<Region> region );
static void set_session_timecode( Session *s, AAF_Iface *aafi );




static void usage()
{
	// help2man compatible format (standard GNU help-text)
	printf (UTILNAME " - create a new session based on an AAF file from the commandline.\n\n");
	printf ("Usage: " UTILNAME " [ OPTIONS ] -p <session-path> --aaf <file.aaf>\n\n");
	printf ("Options:\n\n\
  -h, --help                        Display this help and exit.\n\
  -L, --list-templates              List available Ardour templates and exit.\n\
\n\
  -m, --master-channels      <chn>  Master-bus channel count (default 2).\n\
  -r, --sample-rate         <rate>  Sample rate of the new Ardour session (default is AAF).\n\
  -s, --sample-size     <16|24|32>  Audio bit depth of the new Ardour session (default is AAF).\n\
\n\
  -t, --template        <template>  Use given template for new session.\n\
  -p, --session-path        <path>  Where to store the new session folder.\n\
  -n, --session-name        <name>  The new session name. A new folder will be created into session path with that name.\n\
                                    Default is the AAF composition name or file name.\n\
\n\
  -l, --media-location      <path>  Path to AAF media files (when not embedded)\n\
  -c, --media-cache         <path>  Path where AAF embedded media files will be extracted, prior to Ardour import. Default is TEMP.\n\
  -k, --keep-cache                  Do not clear cache. Useful for analysis of extracted audio files.\n\
\n\
  -a, --aaf             <aaf file>  AAF file to load.\n\
\n\
Vendor Options:\n\
\n\
  Davinci Resolve\n\
\n\
  --import-disabled-clips           Import disabled clips (skipped by default)\n\
\n\
  Pro Tools\n\
\n\
  --remove-sample-accurate-edit     Remove clips added by PT to pad to frame boundary.\n\
  --convert-fade-clips              Remove clip fades and replace by real fades.\n\
\n\
\n");

	printf ("\n\
Examples:\n\
" UTILNAME " --session-path /path/to/sessions/ --aaf /path/to/file.aaf\n\
\n");

	printf ("Report bugs to <http://tracker.ardour.org/>\n"
	        "Website: <http://ardour.org/>\n");
	::exit (EXIT_SUCCESS);
}



static void list_templates()
{
	vector<TemplateInfo> templates;
	find_session_templates( templates, false );

	for ( vector<TemplateInfo>::iterator x = templates.begin (); x != templates.end (); ++x ) {
		printf( "%s\n", (*x).name.c_str() );
	}
}



static std::string template_path_from_name( std::string const& name )
{
	vector<TemplateInfo> templates;
	find_session_templates( templates, false );

	for ( vector<TemplateInfo>::iterator x = templates.begin (); x != templates.end (); ++x ) {
		if ( (*x).name == name )
			return (*x).path;
	}

	return "";
}



static Session* create_new_session( string const &dir, string const &state, float samplerate, ARDOUR::SampleFormat bitdepth, int master_bus_chn, string const &template_path )
{
	AudioEngine* engine = AudioEngine::create();

	if ( !engine->set_backend( "None (Dummy)", "Unit-Test", "" ) ) {
		PRINT_E( "Cannot create Audio/MIDI engine.\n" );
		return NULL;
	}

	// engine->set_input_channels( 32 );
	// engine->set_output_channels( 32 );

	if ( engine->set_sample_rate( samplerate ) ) {
		PRINT_E( "Cannot set session's samplerate to %lf.\n", samplerate );
		return NULL;
	}

	if ( engine->start() != 0 ) {
		PRINT_E( "Cannot start Audio/MIDI engine.\n" );
		return NULL;
	}

	string s = Glib::build_filename( dir, state + statefile_suffix );

	// if ( Glib::file_test( dir, Glib::FILE_TEST_EXISTS ) ) {
	// 	PRINT_E( "Session folder already exists '%s'\n", dir.c_str() );
	// 	return NULL;
	// }
  //
	// if ( Glib::file_test( s, Glib::FILE_TEST_EXISTS ) ) {
	// 	PRINT_E( "Session file exists '%s'\n", s.c_str() );
	// 	return NULL;
	// }

	BusProfile  bus_profile;
	BusProfile *bus_profile_ptr = NULL;

	if ( master_bus_chn > 0 ) {
		bus_profile_ptr = &bus_profile;
		bus_profile.master_out_channels = master_bus_chn;
	}

	if ( !template_path.empty() ) {
		bus_profile_ptr = NULL;
	}

	Session* session = new Session( *engine, dir, state, bus_profile_ptr, template_path );

	engine->set_session( session );

	session->config.set_native_file_data_format(bitdepth);


	return session;
}



// static void set_session_video_from_aaf( Session *s, AAF_Iface *aafi )
// {
//   if ( aafi->Video->Tracks && aafi->Video->Tracks->Items ) {
//
// 		aafiVideoClip *videoClip = (aafiVideoClip*)&aafi->Video->Tracks->Items->data;
//
// 		// printf( "\n\n\nGot video Track and Item : %ls\n\n\n", videoClip->Essence->original_file_path/*->Essence->original_file_path*/ );
//     // char origf[PATH_MAX+1];
//     // snprintf(origf, PATH_MAX, "%ls", videoClip->Essence->original_file_path ); // TODOPATH
//     // printf("Looking for : %s\n", strrchr(origf, '/') + 1 );
//
// 		char *file = locate_external_essence_file( aafi, videoClip->Essence->original_file_path, NULL );
//
// 		if ( file != NULL ) {
//       PRINT_I( "Importing video : %s\n", Glib::path_get_basename(string(file)).c_str()/*fop_get_filename(file)*/ );
//
// 			/* get absolute video file path */
// 			std::string absFile (PBD::canonical_path (file));
//
// 			// /* get original mxf video filename */
// 			// char *file_name = remove_file_ext( basename(file), '.', '/' );
// 			//
// 			// /* creates project video folder */
// 			// mkdir( s->session_directory().video_path().c_str(), 0755 );
// 			//
// 			// /* extract mpeg video from original mxf */
// 			// char cmdstr[PATH_MAX*6];
// 			// snprintf( cmdstr, sizeof(cmdstr), "ffmpeg -y -i \"%s\" -c copy -f mpeg2video \"%s/%s.mpg\"", absFile, s->session_directory().video_path().c_str(), file_name );
// 			// //snprintf( cmdstr, sizeof(cmdstr), "ffmpeg -y -i \"%s\" -c copy -map_metadata 0 \"%s/%s.mkv\"", absFile, s->session_directory().video_path().c_str(), file_name );
// 			//
// 			// system(cmdstr);
//
//
// 			/* Add video to Ardour
// 			 * ===================
// 			 * https://github.com/Ardour/ardour/blob/6987196ea18cbf171e22ed62760962576ccb54da/gtk2_ardour/ardour_ui_video.cc#L317
// 			 *
// 			 *	<Videotimeline Filename="/home/agfline/Developpement/ardio/watchfolder/3572607_RUGBY_F2_S65CFA3D0V.mxf" AutoFPS="1" LocalFile="1" OriginalVideoFile="/home/agfline/Developpement/ardio/watchfolder/3572607_RUGBY_F2_S65CFA3D0V.mxf"/>
//             <RulerVisibility timecode="1" bbt="1" samples="0" minsec="0" tempo="1" meter="1" marker="1" rangemarker="1" transportmarker="1" cdmarker="1" videotl="1"/>
// 			 */
//
// 			XMLNode* videoTLnode = new XMLNode( "Videotimeline" );
// 			videoTLnode->set_property( "Filename", absFile/*string(file_name) + ".mpg"*/ );
// 			videoTLnode->set_property( "AutoFPS", true );
// 			videoTLnode->set_property( "LocalFile", true );
// 			videoTLnode->set_property( "OriginalVideoFile", string(absFile) );
// 			videoTLnode->set_property( "id", 51 );
// 			videoTLnode->set_property( "Height", 3 );
// 			videoTLnode->set_property( "VideoOffsetLock", true );
// 			videoTLnode->set_property( "VideoOffset", eu2sample( s->sample_rate(), videoClip->track->Video->tc->edit_rate, (videoClip->pos + videoClip->track->Video->tc->start)) );
//
//       // printf("\n\n\n%li  |  %li\n\n\n", videoClip->pos, videoClip->track->Video->tc->start );
//
// 			XMLNode* videoMONnode = new XMLNode( "Videomonitor" );
// 			videoMONnode->set_property( "active", true );
//
//
//
// 			XMLNode* xjnode = new XMLNode( "XJSettings" );
//
//       XMLNode* xjsetting;
//       xjsetting = xjnode->add_child( "XJSetting" );
//       xjsetting->set_property( "k", "set offset" );
//       xjsetting->set_property( "v", "-90000" ); //videoClip->pos * videoClip->track->Video->tc->edit_rate );
//
//       xjsetting = xjnode->add_child( "XJSetting" );
//       xjsetting->set_property( "k", "osd smpte" );
//       xjsetting->set_property( "v", "95" );
//
//       /* video_monitor.cc
//       <XJSettings>
//         <XJSetting k="window fullscreen" v="on"/>
//         <XJSetting k="set offset" v="-90000"/>
//         <XJSetting k="osd smpte" v="95"/>
//       </XJSettings>
//       */
//
// 			s->add_extra_xml(*xjnode);
// 			s->add_extra_xml(*videoTLnode);
// 			s->add_extra_xml(*videoMONnode);
//
// 			// s->set_dirty();
// 		}
//     else {
//       PRINT_E( "Could not locate video file : %ls\n", videoClip->Essence->original_file_path );
//     }
// 	}
//   else {
//     PRINT_E( "Could not retrieve video from AAF.\n" );
//   }
// }



/*
 * libs/ardour/import.cc
 * - Reimplement since function was removed in 4620d13 : https://github.com/Ardour/ardour/commit/4620d138eefad57bc55e1901d8410c36803ce0d6 -
 */

static std::shared_ptr<AudioTrack> get_nth_audio_track( uint32_t nth, std::shared_ptr<RouteList const> routes )
{
	RouteList rl = *(routes);//(*(routes.reader ()));
	rl.sort( Stripable::Sorter() );

	for ( auto const& r: rl ) {
		std::shared_ptr<AudioTrack> at = std::dynamic_pointer_cast<AudioTrack> (r);
		if ( !at ) {
			continue;
		}
		if ( nth-- == 0 ) {
			return at;
		}
	}
	return std::shared_ptr<AudioTrack> ();
}



static bool import_sndfile_as_region( Session *s, struct aafiAudioEssence *audioEssence, SrcQuality quality, timepos_t &pos, SourceList &sources, ImportStatus &status, vector<std::shared_ptr<Region> > *regions /* boost::shared_ptr<Region> r*/ )
{
  /* https://github.com/Ardour/ardour/blob/365f6d633731229e7bc5d37a5fe2c9107b527e28/libs/ardour/import_pt.cc#L82 */

  wstring ws(audioEssence->usable_file_path);
  string usable_file_path(ws.begin(), ws.end());

  /* Import the source */
  status.paths.clear();
  status.paths.push_back( usable_file_path /*path*/);
  status.current = 1; // TODO
  status.total = 1; // TODO
  status.freeze = false;
  status.quality = quality;
  status.replace_existing_source = false;
  status.split_midi_channels = false;
  status.import_markers = false;
  status.done = false;
  status.cancel = false;

  s->import_files( status );

  status.progress = 1.0;
  sources.clear();

  /* FIXME: There is no way to tell if cancel button was pressed
   * or if the file failed to import, just that one of these occurred.
   * We want status.cancel to reflect the user's choice only
   */
  if ( status.cancel && status.current > 1 )
  {
  	/* Succeeded to import file, assume user hit cancel */
  	return false;
  }
  else if ( status.cancel && status.current == 1 )
  {
  	/* Failed to import file, assume user did not hit cancel */
  	status.cancel = false;
  	return false;
  }

  for ( int i = 0; i < audioEssence->channels; i++ ) {
    sources.push_back( status.sources.at(i) );
  }


  /* build peakfiles */
  for ( SourceList::iterator x = sources.begin(); x != sources.end(); ++x ) {
  	SourceFactory::setup_peakfile( *x, true );
  }



  /* Put the source on a region */
  std::shared_ptr<Region> region;
  string region_name;


  /* take all the sources we have and package them up as a region */

  region_name = region_name_from_path( status.paths.front(), (sources.size() > 1), false );

  /* we checked in import_sndfiles() that there were not too many */

  while ( RegionFactory::region_by_name( region_name ) ) {
  	region_name = bump_name_once( region_name, '.' );
  }

  ws = audioEssence->unique_file_name;
  string unique_file_name( ws.begin(), ws.end() );

  PropertyList proplist;

  proplist.add (ARDOUR::Properties::start,  0 /*eu2sample_fromclip( clip, clip->essence_offset )*/);
  proplist.add (ARDOUR::Properties::length, timecnt_t( sources[0]->length(), pos ));
  proplist.add (ARDOUR::Properties::name, unique_file_name/*region_name*/);
  proplist.add (ARDOUR::Properties::layer, 0);
  proplist.add (ARDOUR::Properties::whole_file, true);
  proplist.add (ARDOUR::Properties::external, true);


  region = RegionFactory::create( sources, proplist );
  (*regions).push_back( region );



  /* NOTE Don't know what that's for */

  // bool use_timestamp;
  // use_timestamp = (pos == -1);
  // if (use_timestamp && boost::dynamic_pointer_cast<AudioRegion>(r)) {
  // 	boost::dynamic_pointer_cast<AudioRegion>(r)->special_set_position(sources[0]->natural_position());
  // }
  //
  //
  // /* if we're creating a new track, name it after the cleaned-up
  //  * and "merged" region name.
  //  */
  //
  // regions.push_back (r);
  // int n = 0;
  //
  // for (vector<boost::shared_ptr<Region> >::iterator r = regions.begin(); r != regions.end(); ++r, ++n) {
  // 	boost::shared_ptr<AudioRegion> ar = boost::dynamic_pointer_cast<AudioRegion> (*r);
  //
  // 	if (use_timestamp) {
  // 		if (ar) {
  //
  // 			/* get timestamp for this region */
  //
  // 			const boost::shared_ptr<Source> s (ar->sources().front());
  // 			const boost::shared_ptr<AudioSource> as = boost::dynamic_pointer_cast<AudioSource> (s);
  //
  // 			assert (as);
  //
  // 			if (as->natural_position() != 0) {
  // 				pos = as->natural_position();
  // 			} else {
  // 				pos = 0;
  // 			}
  // 		} else {
  // 			/* should really get first position in MIDI file, but for now, use 0 */
  // 			pos = 0;
  // 		}
  // 	}
  // }


  return true;
}



static void set_session_range( Session *s, AAF_Iface *aafi )
{
  samplepos_t start = samplepos_t( eu2sample( s->sample_rate(), &aafi->compositionStart_editRate,  aafi->compositionStart ) );
  samplepos_t end   = samplepos_t( eu2sample( s->sample_rate(), &aafi->compositionLength_editRate, aafi->compositionLength ) ) + start;

  s->set_session_extents( timepos_t(start), timepos_t(end) );
}



static std::shared_ptr<Region> create_region( vector<std::shared_ptr<Region> > source_regions, aafiAudioClip *aafAudioClip, SourceList& oneClipSources, aafPosition_t clipOffset, aafRational_t samplerate_r )
{
  wstring ws = aafAudioClip->Essence->unique_file_name;
  string unique_file_name(ws.begin(), ws.end());

  aafPosition_t clipPos = convertEditUnit( aafAudioClip->pos, *aafAudioClip->track->edit_rate, samplerate_r );
  aafPosition_t clipLen = convertEditUnit( aafAudioClip->len, *aafAudioClip->track->edit_rate, samplerate_r );
  aafPosition_t essenceOffset = convertEditUnit( aafAudioClip->essence_offset, *aafAudioClip->track->edit_rate, samplerate_r );

  PropertyList proplist;

  proplist.add( ARDOUR::Properties::start, essenceOffset );
  proplist.add( ARDOUR::Properties::length, clipLen );
  proplist.add( ARDOUR::Properties::name, unique_file_name );
  proplist.add( ARDOUR::Properties::layer, 0 );
  proplist.add( ARDOUR::Properties::whole_file, false );
  proplist.add( ARDOUR::Properties::external, true );

  /* NOTE: region position is set when calling add_region() */

  std::shared_ptr<Region> region = RegionFactory::create( oneClipSources, proplist );


  for ( SourceList::iterator source = oneClipSources.begin(); source != oneClipSources.end(); ++source ) {

    /* position displayed in Ardour source list */
    (*source)->set_natural_position( timepos_t(clipPos + clipOffset) );

    for ( vector<std::shared_ptr<Region>>::iterator region = source_regions.begin(); region != source_regions.end(); ++region ) {
      if ( (*region)->source(0) == *source ) {
        /* Enable "Move to Original Position" */
        (*region)->set_position( timepos_t(clipPos + clipOffset - essenceOffset) );
      }
    }
  }


  return region;
}



static void set_region_gain( aafiAudioClip *aafAudioClip, std::shared_ptr<Region> region )
{
  if ( aafAudioClip->gain && aafAudioClip->gain->flags & AAFI_AUDIO_GAIN_CONSTANT ) {
    std::dynamic_pointer_cast<AudioRegion>(region)->set_scale_amplitude( aafRationalToFloat(aafAudioClip->gain->value[0]) );
  }
  // TODO: What about clip-gain automation ? No support in Ardour ? Convert to track level ?
}



static std::shared_ptr<AudioTrack> prepare_audio_track( aafiAudioTrack *aafTrack, Session *s )
{
  /* Add region to track
   * ===================
   * https://github.com/Ardour/ardour/blob/365f6d633731229e7bc5d37a5fe2c9107b527e28/libs/ardour/import_pt.cc#L327
   */

  /* Use existing track if possible */
  std::shared_ptr<AudioTrack> track = get_nth_audio_track( (aafTrack->number-1), s->get_routes() );

  /* Or create a new track if needed */
  if ( !track ) {

    wstring ws_track_name = std::wstring( aafTrack->name );
    string track_name = string(ws_track_name.begin(), ws_track_name.end());

    PRINT_I( "Track number %i (%s) does not exist. Adding new track.\n", aafTrack->number, track_name.c_str() );

    // TODO: second argument is "output_channels". How should it be set ?
    list<std::shared_ptr<AudioTrack> > at( s->new_audio_track( aafTrack->format, 2, 0, 1, track_name, PresentationInfo::max_order, Normal ) );

    if ( at.empty() ) {
      PRINT_E( "Could not create new audio track.\n" );
      ::exit( EXIT_FAILURE );
    }

    track = at.back();
  }

  return track;
}



static void set_region_fade( aafiAudioClip *aafAudioClip, std::shared_ptr<Region> region )
{
  /* Set fades if any
   * ================
   *
   * https://github.com/Ardour/ardour/blob/b84c99639f0dd28e210ed9c064429c17014093a7/libs/ardour/ardour/types.h#L705
   *
   * enum FadeShape {
   *   FadeLinear,
   *   FadeFast,
   *   FadeSlow,
   *   FadeConstantPower,
   *   FadeSymmetric,
   * };
   *
   * https://github.com/Ardour/ardour/blob/365f6d633731229e7bc5d37a5fe2c9107b527e28/libs/ardour/ardour/audioregion.h#L143
   * https://github.com/Ardour/ardour/blob/365f6d633731229e7bc5d37a5fe2c9107b527e28/libs/temporal/temporal/types.h#L39
   */

  aafiTransition *fadein  = get_fadein( aafAudioClip->Item );
  aafiTransition *fadeout = get_fadeout( aafAudioClip->Item );

  if ( fadein == NULL ) {
    fadein = get_xfade( aafAudioClip->Item );
  }

  FadeShape fade_shape;
  samplecnt_t fade_len;

  if ( fadein != NULL ) {
    fade_shape = (fadein->flags & AAFI_INTERPOL_NONE)     ? FadeLinear :
                 (fadein->flags & AAFI_INTERPOL_LINEAR)   ? FadeLinear :
                 (fadein->flags & AAFI_INTERPOL_LOG)      ? FadeSymmetric :
                 (fadein->flags & AAFI_INTERPOL_CONSTANT) ? FadeConstantPower :
                 (fadein->flags & AAFI_INTERPOL_POWER)    ? FadeConstantPower :
                 (fadein->flags & AAFI_INTERPOL_BSPLINE)  ? FadeLinear :
                 FadeLinear;
    fade_len = eu2sample_fromclip( aafAudioClip, fadein->len );

    std::dynamic_pointer_cast<AudioRegion>(region)->set_fade_in( fade_shape, fade_len );
  }

  if ( fadeout != NULL ) {
    fade_shape = (fadeout->flags & AAFI_INTERPOL_NONE)     ? FadeLinear :
                 (fadeout->flags & AAFI_INTERPOL_LINEAR)   ? FadeLinear :
                 (fadeout->flags & AAFI_INTERPOL_LOG)      ? FadeSymmetric :
                 (fadeout->flags & AAFI_INTERPOL_CONSTANT) ? FadeConstantPower :
                 (fadeout->flags & AAFI_INTERPOL_POWER)    ? FadeConstantPower :
                 (fadeout->flags & AAFI_INTERPOL_BSPLINE)  ? FadeLinear :
                 FadeLinear;
    fade_len = eu2sample_fromclip( aafAudioClip, fadeout->len );

    std::dynamic_pointer_cast<AudioRegion>(region)->set_fade_out( fade_shape, fade_len );
  }
}



static void set_session_timecode( Session *s, AAF_Iface *aafi )
{
  uint16_t aaftc2 = aafi->Audio->tc->fps;
  // aafRational_t *aaftc1 = ( aafi->Video ) ? ( aafi->Video->Essences ) ? aafi->Video->Essences->framerate : NULL : NULL;
  TimecodeFormat ardourtc;

  /*
   *  The following is based on adobe premiere pro's AAF.
   *  Fractional timecodes are never explicitly set into tc->fps, so we deduce
   *  them based on edit_rate value.
   *
   *  Available timecodes in ardour (libs/ardour/enums.cc) :
   *
   *    	REGISTER_ENUM (timecode_23976);
   *     	REGISTER_ENUM (timecode_24);
   *    	REGISTER_ENUM (timecode_24976);
   *    	REGISTER_ENUM (timecode_25);
   *    	REGISTER_ENUM (timecode_2997);
   *    	REGISTER_ENUM (timecode_2997drop);
   *    	REGISTER_ENUM (timecode_30);
   *    	REGISTER_ENUM (timecode_30drop);
   *    	REGISTER_ENUM (timecode_5994);
   *   	  REGISTER_ENUM (timecode_60);
   *
   *
   *  TODO: Why should we set TC based on aafi->Video->Essences->framerate ?
   *  Disable until we found a good reason.
   */

  // if ( aaftc1 ) {
  //
  //   if ( aaftc1->numerator   == 24000 &&
  //        aaftc1->denominator ==  1001 )
  //   {
  //     ardourtc = timecode_23976;
  //   }
  //   else
  //   if ( aaftc1->numerator   == 24 &&
  //        aaftc1->denominator ==  1 )
  //   {
  //     ardourtc = timecode_24;
  //   }
  //   else
  //   if ( aaftc1->numerator   == 25 &&
  //        aaftc1->denominator ==  1 )
  //   {
  //     ardourtc = timecode_25;
  //   }
  //   else
  //   if ( aaftc1->numerator   == 29000 &&
  //        aaftc1->denominator ==  1001 )
  //   {
  //     ardourtc = timecode_2997;
  //   }
  //   else
  //   if ( aaftc1->numerator   == 30 &&
  //        aaftc1->denominator ==  1 )
  //   {
  //     ardourtc = timecode_30;
  //   }
  //   else
  //   if ( aaftc1->numerator   == 59000 &&
  //        aaftc1->denominator ==  1001 )
  //   {
  //     ardourtc = timecode_5994;
  //   }
  //   else
  //   if ( aaftc1->numerator   == 60 &&
  //        aaftc1->denominator ==  1 )
  //   {
  //     ardourtc = timecode_60;
  //   }
  //   else {
  //     PRINT_E( "Unknown AAF timecode fps : %i/%i.\n", aaftc1->numerator, aaftc1->denominator );
  //     return;
  //   }
  // }
  // else {

    switch ( aaftc2 ) {

      case 24:
        if ( aafi->Audio->tc->edit_rate->numerator   == 24000 &&
             aafi->Audio->tc->edit_rate->denominator == 1001 )
        {
          ardourtc = timecode_23976;
        }
        else {
          ardourtc = timecode_24;
        }
        break;

      case 25:
        if ( aafi->Audio->tc->edit_rate->numerator   == 25000 &&
             aafi->Audio->tc->edit_rate->denominator == 1001 )
        {
          ardourtc = timecode_24976;
        }
        else {
          ardourtc = timecode_25;
        }
        break;

      case 30:
        if ( aafi->Audio->tc->edit_rate->numerator   == 30000 &&
             aafi->Audio->tc->edit_rate->denominator == 1001 )
        {
          if ( aafi->Audio->tc->drop ) {
            ardourtc = timecode_2997drop;
          }
          else {
            ardourtc = timecode_2997;
          }
        }
        else {
          if ( aafi->Audio->tc->drop ) {
            ardourtc = timecode_30drop;
          }
          else {
            ardourtc = timecode_30;
          }
        }
        break;

      case 60:
        if ( aafi->Audio->tc->edit_rate->numerator   == 60000 &&
             aafi->Audio->tc->edit_rate->denominator == 1001 )
        {
          ardourtc = timecode_5994;
        }
        else {
          ardourtc = timecode_60;
        }
        break;


      default:
        PRINT_E( "Unknown AAF timecode fps : %i.\n", aaftc2 );
        return;
    }
  // }

  s->config.set_timecode_format( ardourtc );
}






#ifdef _WIN32
  #include <io.h>
  // #define access _access_s
  // #define DIR_SEP '\\'
  // #define DIR_SEP_STR "\\"
#else
  #include <paths.h>
  // #include <unistd.h>
  // #define DIR_SEP '/'
  // #define DIR_SEP_STR "/"
#endif


int prepare_cache( AAF_Iface *aafi, string *media_cache_path ) {

  if ( !(*media_cache_path).empty() ) {
    /* if media cache is not empty, user forced cache path with --media-cache */
    return 0;
  }

  const char *tmppath = g_get_tmp_dir();

  if ( !aafi->compositionName || aafi->compositionName[0] == 0x00 ) {
    *media_cache_path = g_build_path( G_DIR_SEPARATOR_S, tmppath, g_basename( aafi->aafd->cfbd->file ), NULL ); //+ string(DIR_SEP_STR);
  }
  else {

    int compoNameLen = wcslen(aafi->compositionName) + 1;
    char *compoName = (char*)malloc( compoNameLen );

    wcstombs( compoName, aafi->compositionName, compoNameLen );

    /*
     * sanitize dir name
     * https://stackoverflow.com/a/31976060
     */
    for ( int i = 0; i < (compoNameLen-1); i++ ) {
      char c = compoName[i];
      if ( c == '/' ||
           c == '<' ||
           c == '>' ||
           c == ':' ||
           c == '"' ||
           c == '|' ||
           c == '?' ||
           c == '*' ||
           c == '\\' ||
           c < 0x30 ||
           c > 0x7a )
      {
        compoName[i] = '_';
      }
    }

    *media_cache_path = g_build_path( G_DIR_SEPARATOR_S, tmppath, compoName, NULL );

    g_free( compoName );
  }


  int i = 0;
  string testdir = *media_cache_path;

  while ( g_file_test( testdir.c_str(), G_FILE_TEST_EXISTS ) )
    testdir = *media_cache_path + "_" + to_string(i++);

  *media_cache_path = testdir;


  if ( mkdir( (*media_cache_path).c_str() , 0760 ) < 0 ) {
  // if ( g_mkdir_with_parents( (*media_cache_path).c_str(), 0760 ) < 0 ) {
    PRINT_E( "Could not create cache directory at '%s' : %s\n", (*media_cache_path).c_str(), strerror(errno) );
    return -1;
  }

  return 0;
}



void clear_cache( AAF_Iface *aafi, string media_cache_path )
{
  aafiAudioEssence *audioEssence = NULL;

  foreachEssence( audioEssence, aafi->Audio->Essences ) {

// #ifdef _WIN32
//     wchar_t *filepath = audioEssence->usable_file_path;
// #else
    char *filepath = (char*)malloc( wcslen(audioEssence->usable_file_path) + 1 );
    snprintf( filepath, wcslen(audioEssence->usable_file_path) + 1, "%ls", audioEssence->usable_file_path );
// #endif

    // if ( access( filepath, 0 ) == 0 ) {
    if ( g_file_test( filepath, G_FILE_TEST_EXISTS ) ) {
      PRINT_W( "Would have removed from cache : %s\n", filepath );

      // if ( remove( filepath ) < 0 ) {
      //   PRINT_E( "Failed to remove a file from cache (%s) : %s\n", filepath, strerror(errno) );
      // }
    }
    else {
      PRINT_E( "Missing a file from cache (%s) : %s\n", filepath, strerror(errno) );
    }

#ifndef _WIN32
    free( filepath );
#endif
  }

  if ( rmdir( media_cache_path.c_str() ) < 0 ) {
    PRINT_E( "Failed to remove cache directory (%s) : %s\n", media_cache_path.c_str(), strerror(errno) );
  }
}


int main( int argc, char* argv[] )
{
  ARDOUR::SampleFormat bitdepth = ARDOUR::FormatInt24;
  int    samplesize     = 0;
	int    samplerate     = 0;
	int    master_bus_chn = 2;
	string template_path;
  string output_folder;
	string session_name;
  string media_location_path;
  string media_cache_path;
  int    keep_cache = 0;
  string aaf_file;
  uint32_t aaf_resolve_options = 0;
  uint32_t aaf_protools_options = 0;
  // bool replace_session_if_exists = false;

  printf( "using libaaf %s\n", LIBAAF_VERSION );

	const char* optstring = "hLm:r:s:t:p:n:l:c:a:";

	const struct option longopts[] = {
    { "help",            no_argument,       0, 'h' },

		{ "list-templates",  no_argument,       0, 'L' },

		{ "master-channels", required_argument, 0, 'm' },

		{ "sample-rate",     required_argument, 0, 'r' },
    { "sample-size",     required_argument, 0, 's' },

		{ "template",        required_argument, 0, 't' },
    { "session-path",    required_argument, 0, 'p' },
		{ "session-name",    required_argument, 0, 'n' },

    { "media-location",  required_argument, 0, 'l' },
    { "media-cache",     required_argument, 0, 'c' },
    { "keep-cache",      required_argument, 0, 'k' },

    { "aaf",             required_argument, 0, 'a' },

    { "import-disabled-clips",       no_argument, 0, 0x01 },
    { "remove-sample-accurate-edit", no_argument, 0, 0x02 },
    { "convert-fade-clips",          no_argument, 0, 0x03 },

    // { "replace-session-if-exists", no_argument, 0, 0x01 }
	};

	int c = 0;

	while ( EOF != (c = getopt_long( argc, argv, optstring, longopts, (int*)0 )) ) {

		switch (c) {

      case 'h':
        usage();
        break;

      case 'L':
        list_templates();
        exit( EXIT_SUCCESS );
        break;

      case 'm':
        master_bus_chn = atoi(optarg);
        /* TODO check min / max */
        break;

      case 'r':
        samplerate = atoi(optarg);

        if ( samplerate < 44100 || samplerate > 192000 ) {
          PRINT_E("Invalid sample rate (%s). Sample rate must be between 44100 and 192000.\n", optarg);
          ::exit( EXIT_FAILURE );
        }
        break;

      case 's':
        samplesize = atoi(optarg);

        if ( samplesize != 16 && samplesize != 24 && samplesize != 32 ) {
          PRINT_E("Invalid sample size (%s). Sample size must be either 16, 24 or 32.\n", optarg);
          ::exit( EXIT_FAILURE );
        }
        break;

			case 't':
				template_path = template_path_from_name( optarg );
				if ( template_path.empty() ) {
					cerr << "Invalid (non-existent) template:" << optarg << "\n";
					::exit( EXIT_FAILURE );
				}
				break;

      case 'p':
        output_folder = string(optarg);
        break;

			case 'n':
				session_name = string(optarg);
				break;


      case 'l':
        media_location_path = string(optarg);
        break;

      case 'c':
        media_cache_path = string(optarg);
        break;

      case 'k':
        keep_cache = 1;
        break;

      case 'a':
        aaf_file = string(optarg);
        break;

      case 0x01:
        aaf_resolve_options |= RESOLVE_INCLUDE_DISABLED_CLIPS;
        break;

      case 0x02:
        aaf_protools_options |= PROTOOLS_REMOVE_SAMPLE_ACCURATE_EDIT;
        break;

      case 0x03:
        aaf_protools_options |= PROTOOLS_REPLACE_CLIP_FADES;
        break;

      default:
				cerr << "Error: unrecognized option. See --help for usage information.\n";
				::exit( EXIT_FAILURE );
				break;
		}
	}

  int missing_param = 0;

	// if ( template_path.empty() )
	// {
	// 	PRINT_E( "Missing template. Use --template parameter.\n" );
  //   missing_param = 1;
	// }

  if ( output_folder.empty() ) {
    PRINT_E( "Missing session path. Use --session-path parameter.\n" );
    missing_param = 1;
  }

  if ( aaf_file.empty() ) {
    PRINT_E( "Missing AAF file. Use --aaf parameter.\n" );
    missing_param = 1;
  }


  if ( missing_param ) {
    ::exit( EXIT_FAILURE );
  }




	AAF_Iface *aafi = aafi_alloc( NULL );

  aafi->ctx.options.verb = VERB_DEBUG;
  aafi->ctx.options.trace = 1;
  aafi->ctx.options.resolve = aaf_resolve_options;
  aafi->ctx.options.protools = aaf_protools_options;
  aafi->ctx.options.media_location = (media_location_path.empty()) ? NULL : strdup( media_location_path.c_str() );

  if ( aafi_load_file( aafi, aaf_file.c_str() ) ) {
		PRINT_E( "Could not load AAF file.\n" );
		::exit( EXIT_FAILURE );
	}


  if ( prepare_cache( aafi, &media_cache_path ) ) {
    PRINT_E( "Could not prepare media cache path.\n" );
    ::exit( EXIT_FAILURE );
  }

  printf( "Media Cache : %s\n\n", media_cache_path.c_str() );

  /*
   * At this stage, AFF was loaded and parsed,
   * so we can print a few things first.
   */

  aaf_dump_Header( aafi->aafd );

  aaf_dump_Identification( aafi->aafd );

  printf( " Composition Name       : %ls\n", aafi->compositionName );
  printf( " Composition Start      : %lu\n", eu2sample( aafi->Audio->samplerate, &aafi->compositionStart_editRate,  aafi->compositionStart ) );
  printf( " Composition End        : %lu\n", eu2sample( aafi->Audio->samplerate, &aafi->compositionLength_editRate, aafi->compositionLength ) + eu2sample( aafi->Audio->samplerate, &aafi->compositionStart_editRate,  aafi->compositionStart ) );
  printf( " Composition SampleRate : %li Hz\n",  aafi->Audio->samplerate );
  printf( " Composition SampleSize : %i bits\n", aafi->Audio->samplesize );
  printf( "\n" );


  if ( !samplerate ) {
    PRINT_I( "Using AAF file sample rate : %li Hz\n", aafi->Audio->samplerate );
    samplerate = aafi->Audio->samplerate;
  }
  else {
    PRINT_I( "Ignoring AAF file sample rate (%li Hz), using user defined : %i Hz\n", aafi->Audio->samplerate, samplerate );
  }


  aafRational_t samplerate_r;

  samplerate_r.numerator = samplerate;
  samplerate_r.denominator = 1;


  if ( !samplesize ) {
    PRINT_I( "Using AAF file bit depth : %i bits\n", aafi->Audio->samplesize );
    samplesize = aafi->Audio->samplesize;
  }
  else {
    PRINT_I( "Ignoring AAF file bit depth (%i bits), using user defined : %i bits\n", aafi->Audio->samplesize, samplesize );
  }

  switch ( samplesize ) {
    case 16:  bitdepth = ARDOUR::FormatInt16; break;
    case 24:  bitdepth = ARDOUR::FormatInt24; break;
    case 32:  bitdepth = ARDOUR::FormatFloat; break;
    default:
      PRINT_E( "Invalid sample size (%i). Sample size must be either 16, 24 or 32.\n", samplesize );
      ::exit( EXIT_FAILURE );
  }


	if ( session_name.empty() ) {

    if ( aafi->compositionName ) {
      wstring ws_session_name = std::wstring( aafi->compositionName );
      session_name = string(ws_session_name.begin(), ws_session_name.end());
      PRINT_I( "Using AAF composition name for Ardour session name : %ls\n", aafi->compositionName );
    }
    else {
      /*
       * Code from gtk2_ardour/utils_videotl.cc
       * VideoUtils::strip_file_extension()
       */
      std::string infile = Glib::path_get_basename(string(aafi->aafd->cfbd->file));
      char *ext, *bn = strdup(infile.c_str());
      if ((ext=strrchr(bn, '.'))) {
        if (!strchr(ext, G_DIR_SEPARATOR)) {
          *ext = 0;
        }
      }
      session_name = std::string(bn);
      free(bn);

      PRINT_I( "AAF has no composition name, using AAF file name for Ardour session name : %s\n", session_name.c_str() );
    }
	}

  if ( Glib::file_test( string(output_folder + G_DIR_SEPARATOR + session_name), Glib::FILE_TEST_IS_DIR ) ) {
    // if ( !replace_session_if_exists ) {
  		PRINT_E( "Session folder already exists '%s'\n", string(output_folder + G_DIR_SEPARATOR + session_name).c_str() );
  		::exit( EXIT_FAILURE );
    // }
    // else {
    //   PRINT_I( "Overwriting existing Ardour session : %s \n", string(output_folder + G_DIR_SEPARATOR + session_name).c_str() );
    // }
	}



	SessionUtils::init();
	Session *s = NULL;


	try {
		s = create_new_session( output_folder + G_DIR_SEPARATOR + session_name /*session_file*/, session_name, samplerate, bitdepth, master_bus_chn, template_path );
	} catch ( ARDOUR::SessionException& e ) {
		// cerr << "Error: " << e.what () << "\n";
    PRINT_E( "Could not create ardour session : %s\n", e.what() );
		SessionUtils::unload_session(s);
		SessionUtils::cleanup();
		aafi_release( &aafi );
		::exit( EXIT_FAILURE );
	} catch (...) {
		// cerr << "Error: unknown exception.\n";
    PRINT_E( "Could not create ardour session.\n" );
		SessionUtils::unload_session(s);
		SessionUtils::cleanup();
		aafi_release( &aafi );
		::exit( EXIT_FAILURE );
	}




  /*
   *
   *  Extract audio files and import as sources
   *  libs/ardour/import_pt.cc#L188
   *
   */

	SourceList oneClipSources;
  ARDOUR::ImportStatus import_status;
  vector<std::shared_ptr<Region> > source_regions;
  timepos_t pos = timepos_t::max (Temporal::AudioTime);

  aafiAudioEssence *audioEssence = NULL;


	foreachEssence( audioEssence, aafi->Audio->Essences ) {

    /*
     *  If we extract embedded essences to `s->session_directory().sound_path()` then we end up with a duplicate on import.
     *  So we extract essence to a cache folder
     *  TODO: clear cache
     */

		if ( audioEssence->is_embedded ) {
      if ( media_cache_path.empty() ) {
        PRINT_E( "Could not extract audio file from AAF : media cache was not set.\n" );
        continue;
      }
      if ( aafi_extract_audio_essence( aafi, audioEssence, media_cache_path.c_str(), NULL, 0 ) < 0 ) {
        PRINT_E( "Could not extract audio file '%ls' from AAF.\n", audioEssence->unique_file_name );
        continue; // TODO or fail ?
      }
		}
    else {

      if ( !audioEssence->usable_file_path ) {
        PRINT_E( "Could not locate external audio file : '%ls'\n", audioEssence->original_file_path );
        continue;
      }
    }

    if ( !import_sndfile_as_region( s, audioEssence, SrcBest, pos, oneClipSources, import_status, &source_regions ) ) {
      PRINT_E( "Could not import '%ls' to session.\n", audioEssence->unique_file_name );
      continue; // TODO or fail ?
    }

    audioEssence->user = new SourceList(oneClipSources);

    PRINT_I( "Source file '%ls' successfully imported to session.\n", audioEssence->unique_file_name );
	}

  oneClipSources.clear();



  /*
   *  Get timeline offset as sample value
   */
  aafPosition_t sessionStart = convertEditUnit( aafi->compositionStart, aafi->compositionStart_editRate, samplerate_r );



  /*
   *
   *  Create all audio clips
   *
   */

  aafiAudioTrack   *aafAudioTrack = NULL;
  aafiTimelineItem *aafAudioItem  = NULL;
  aafiAudioClip    *aafAudioClip  = NULL;

  foreach_audioTrack( aafAudioTrack, aafi ) {

    std::shared_ptr<AudioTrack> track = prepare_audio_track( aafAudioTrack, s );

    foreach_Item( aafAudioItem, aafAudioTrack ) {

      if ( aafAudioItem->type != AAFI_AUDIO_CLIP ) {
        continue;
      }

      aafAudioClip = (aafiAudioClip*)&aafAudioItem->data;

      if ( aafAudioClip->Essence == NULL ) {
        PRINT_E( "AAF clip has no essence\n" );
        continue;
      }


      /* converts whatever edit_rate clip is in, to samples */
      aafPosition_t clipPos = convertEditUnit( aafAudioClip->pos, *aafAudioClip->track->edit_rate, samplerate_r );

      PRINT_I( "Importing new clip %ls [%+05.1lf dB] on track %i @%s\n",
        aafAudioClip->Essence->unique_file_name,
        (( aafAudioClip->gain && aafAudioClip->gain->flags & AAFI_AUDIO_GAIN_CONSTANT ) ? 20 * log10( aafRationalToFloat( aafAudioClip->gain->value[0] ) ) : 0),
        aafAudioClip->track->number,
        timecode_format_sampletime( (clipPos + sessionStart), samplerate, aafAudioClip->track->Audio->tc->fps, false ).c_str()
      );


      aafiAudioEssence *audioEssence = aafAudioClip->Essence;

      if ( !audioEssence || !audioEssence->user ) {
        PRINT_E( "Could not create new region for clip %ls : Missing audio essence\n", aafAudioClip->Essence->unique_file_name );
        continue;
      }


      SourceList *oneClipSources = static_cast<SourceList*>(audioEssence->user);

      if ( oneClipSources->size() == 0 ) {
        PRINT_E( "Could not create new region for clip %ls : Region has no source\n", aafAudioClip->Essence->unique_file_name );
        continue;
      }


      std::shared_ptr<Region> region = create_region( source_regions, aafAudioClip, *oneClipSources, sessionStart, samplerate_r );

      if ( !region ) {
        PRINT_E( "Could not create new region for clip %ls\n", aafAudioClip->Essence->unique_file_name );
        ::exit( EXIT_FAILURE );
      }

      /* Put region on track */

      std::shared_ptr<Playlist> playlist = track->playlist();

      playlist->add_region( region, timepos_t( clipPos + sessionStart ) );


      set_region_gain( aafAudioClip, region );

      set_region_fade( aafAudioClip, region );
    }
  }


  foreachEssence( audioEssence, aafi->Audio->Essences ) {
    if ( audioEssence && audioEssence->user ) {
      static_cast<SourceList*>(audioEssence->user)->clear();
    }
  }

  source_regions.clear();

  if ( !keep_cache ) {
    clear_cache( aafi, media_cache_path );
  }


  /*
   *  Set Session Range
   */

  set_session_range( s, aafi );



  /*
   *  Import Video from AAF
   *  SegFault !
   */

	// set_session_video_from_aaf( s, aafi );



  /*
   *  Import Video from AAF
   */

  set_session_timecode( s, aafi );




  import_status.progress = 1.0;
	import_status.done = true;
	s->save_state("");
	import_status.sources.clear();
  import_status.all_done = true;



  /* NOTE: we need to build this before releasing session ! */
  string session_file_path = s->session_directory().root_path() + G_DIR_SEPARATOR + session_name + string(".ardour");



	SessionUtils::unload_session(s);
	SessionUtils::cleanup();


	aafi_release( &aafi );


	return 0;
}