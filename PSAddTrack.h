//
//  PSAddTrack.h
//  Gusto
//
//  Created by taybin on 3/4/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#import <Cocoa/Cocoa.h>

#include <ardour/types.h>

#import "PSSessionDocument.h"

@interface PSAddTrack : NSObject {
	int nTracks;
	BOOL track;
	
	IBOutlet NSPanel *panel;
	IBOutlet NSPopUpButton *channel_combo;
	IBOutlet NSPopUpButton *track_mode;
	
	PSSessionDocument *document;
}

- (IBAction)ok:(id)sender;
- (IBAction)cancel:(id)sender;

- (int)nTracks;
- (void)setNTracks:(int)n;
- (BOOL)track;
- (void)setTrack:(BOOL)yn;

- (int)inputChannels;
- (ARDOUR::TrackMode)mode;

- (PSSessionDocument *)document;
- (void)setDocument:(PSSessionDocument *)doc;

- (void)doit:(PSSessionDocument *)doc;

- (void)didEndSheet:(NSWindow *)sheet returnCode:(int)returnCode contextInfo:(void *)contextInfo;

@end
