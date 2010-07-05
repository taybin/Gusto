//
//  PSEditorTrack.mm
//  Gusto
//
//  Created by taybin on 3/8/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#include <ardour/audio_track.h>
#include <ardour/route.h>
#include <ardour/session.h>

#import "PSEditorTrack.h"


@implementation PSEditorTrack

- (id)initWithRoute:(boost::shared_ptr<ARDOUR::Route>)r withSession:(ARDOUR::Session*)s
{
	self = [super initWithRoute:r withSession:s];
	if (self) {
		[NSBundle loadNibNamed:@"TrackControl" owner:self];
	}
	
	return self;
}

- (void)dealloc
{
	[trackControl release];
	[super dealloc];
}


- (NSView*)trackControl
{
	return trackControl;
}

@end
