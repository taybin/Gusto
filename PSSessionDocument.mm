//
//  PSSessionDocument.mm
//  Gusto
//
//  Created by Taybin Rutkin on 2/20/07.
//  Copyright Penguin Sounds 2007 . All rights reserved.
//

#include <sigc++/sigc++.h>

#include <ardour/configuration.h>
#include <ardour/session.h>
#include <ardour/route.h>
#include <ardour/audioengine.h>

#import "PSAppController.h"
#import "PSNewDialogController.h"
#import "PSSessionDocument.h"
#import "PSAddTrack.h"
#import "PSEditorTrack.h"
#import "PSEditorWindowController.h"
#import "PSMixerWindowController.h"

#include "objcsigcglue.h"

@implementation PSSessionDocument

- (id)init
{
    self = [super init];
    if (self) {
		sess = 0;
    }
    return self;
}

- (void) dealloc {
	delete sess;
	
	[super dealloc];
}

- (void)makeWindowControllers
{
	PSEditorWindowController *editor = [[PSEditorWindowController alloc] initWithSession:sess];
	[self addWindowController:editor];
	
	PSMixerWindowController *mixer = [[PSMixerWindowController alloc] initWithSession:sess];
	[self addWindowController:mixer];
	[mixer showWindow:self];
}

- (void)windowControllerDidLoadNib:(NSWindowController *) aController
{
    [super windowControllerDidLoadNib:aController];
    // Add any code here that needs to be executed once the windowController has loaded the document's window.
}

- (BOOL)writeToURL:(NSURL *)absoluteURL ofType:(NSString *)typeName forSaveOperation:(NSSaveOperationType)saveOperation originalContentsURL:(NSURL *)absoluteOriginalContentsURL error:(NSError **)outError
{
	NSLog(@"writeToURL. NOT REACHED");
	
    return YES;
}

- (void)saveSession
{
	sess->save_state ([[[[self fileURL] path] lastPathComponent] UTF8String]);
	[self updateChangeCount:NSChangeCleared];
}

- (BOOL)readFromURL:(NSURL *)absoluteURL ofType:(NSString *)typeName error:(NSError **)outError
{
    // Insert code here to read your document from the given data of the specified type.  If the given outError != NULL, ensure that you set *outError when returning NO.

	NSLog([absoluteURL path]);
	
	ARDOUR::AudioEngine *eng = [[PSAppController sharedManager] engine];
	
	try {
		sess = new ARDOUR::Session (*eng, [[absoluteURL path] fileSystemRepresentation], [[[absoluteURL path] lastPathComponent] UTF8String], 0);
	} catch (...) {
		NSLog(@"Session did not load correctly");
		return NO;
	}
		
    return YES;
}

- (IBAction)addTrack:(id)sender
{
	PSAddTrack *at = [[PSAppController sharedManager] addTrackController];
	[at doit:self];
}

- (void)tracksAdded
{
	PSAddTrack *at = [[PSAppController sharedManager] addTrackController];
	
	uint32_t input_chan = [at inputChannels];
	uint32_t output_chan;
	
	ARDOUR::AutoConnectOption oac = ARDOUR::Config->get_output_auto_connect();
	if (oac & ARDOUR::AutoConnectMaster) {
		output_chan = (sess->master_out() ? sess->master_out()->n_inputs() : input_chan);
	}
	
	try {
		if ([at track]) {
			sess->new_audio_track (input_chan, output_chan, [at mode], [at nTracks]);
		} else {
			sess->new_audio_route (input_chan, output_chan, [at nTracks]);
		}
	} catch (...) {
		// TODO can we automate this?
		NSAlert *alert = [[NSAlert alloc] init];
		[alert addButtonWithTitle:@"OK"];
		[alert setMessageText:@"Could not create all tracks/busses"];
		[alert setInformativeText:@"Save your session, exit, and try to restart Jack with more ports"];
		[alert setAlertStyle:NSWarningAlertStyle];
	}
	
	[self updateChangeCount:NSChangeDone];
}

@end
