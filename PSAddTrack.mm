//
//  PSAddTrack.mm
//  Gusto
//
//  Created by taybin on 3/4/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#import "PSAddTrack.h"
#import "PSSessionDocument.h"

#include "objcsigcglue.h"

@implementation PSAddTrack

- (id)init
{
	self = [super init];
	
	[NSBundle loadNibNamed:@"AddTrack" owner:self];
	
	[self setTrack:YES];
	[self setNTracks:1];
	
	return self;
}

- (IBAction)ok:(id)sender
{
	[[self document] tracksAdded];

	[NSApp endSheet:panel];
}

- (IBAction)cancel:(id)sender
{
    [NSApp endSheet:panel];
}

- (int)nTracks
{
	return nTracks;
}

- (void)setNTracks:(int)n
{
	nTracks = n;
}

- (BOOL)track
{
	return track;
}

- (void)setTrack:(BOOL)yn
{
	track = yn;
}

- (int)inputChannels
{
	return [[channel_combo selectedItem] tag];
}

- (ARDOUR::TrackMode)mode
{
	if ([[track_mode selectedItem] tag] == 0) {
		return ARDOUR::Normal;
	} else {
		return ARDOUR::Destructive;
	}
}

- (PSSessionDocument *)document
{
	return document;
}

- (void)setDocument:(PSSessionDocument *)doc
{
	document = doc;
}

- (void)doit:(PSSessionDocument *)doc
{
	[self setDocument:doc];	
	[NSApp beginSheet: panel
			modalForWindow: [doc windowForSheet]
			modalDelegate: self
			didEndSelector: @selector(didEndSheet:returnCode:contextInfo:)
			contextInfo: nil];
}

- (void)didEndSheet:(NSWindow *)sheet returnCode:(int)returnCode contextInfo:(void *)contextInfo
{
    [sheet orderOut:self];
}

@end

