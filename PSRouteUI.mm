//
//  PSRouteUI.mm
//  Gusto
//
//  Created by Taybin on 3/16/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#include <ardour/audio_track.h>
#include <ardour/route.h>
#include <ardour/session.h>

#import "PSRouteUI.h"
#import "PSToggleButton.h"


@implementation PSRouteUI

- (id)initWithRoute:(boost::shared_ptr<ARDOUR::Route>)r withSession:(ARDOUR::Session*)s
{
	self = [super init];
	if (self) {
		route = r;
		session = s;
	}
	
	return self;
}

- (void)awakeFromNib
{
	if ([self isAudioTrack]) {
		[recordBtn setEnabled:YES];
		route->record_enable_changed.connect (sigc::bind (sigc::ptr_fun(recordDisplayGlue), self));
		[recordBtn setShiftAction:@selector(recordShiftClicked:)];
		[self updateRecord];
	}
	
	route->mute_changed.connect (sigc::bind (sigc::ptr_fun(muteDisplayGlue), self));
	route->solo_changed.connect (sigc::bind (sigc::ptr_fun(soloDisplayGlue), self));
	route->name_changed.connect (sigc::bind (sigc::ptr_fun(nameDisplayGlue), self));
	
	[soloBtn setShiftAction:@selector(soloShiftClicked:)];
	[muteBtn setShiftAction:@selector(muteShiftClicked:)];
	
	[self updateMute];
	[self updateSolo];
	[self setName:[NSString stringWithUTF8String:route->name().c_str()]];
}

- (BOOL)isAudioTrack
{
	return dynamic_cast<ARDOUR::AudioTrack*>(route.get()) != 0;
}

- (IBAction)mutePressed:(id)sender
{
	route->set_mute([muteBtn state], self);
}

- (IBAction)muteShiftClicked:(id)sender
{
	session->set_all_mute ([muteBtn state]);
}

- (void)updateMute
{
	[muteBtn setState:route->muted()];
}

- (IBAction)soloPressed:(id)sender
{
	route->set_solo([soloBtn state], self);
}

- (IBAction)soloShiftClicked:(id)sender
{
	session->set_all_solo ([soloBtn state]);
}

- (void)updateSolo
{
	[soloBtn setState:route->soloed()];
}

- (IBAction)recordPressed:(id)sender
{
	route->set_record_enable([recordBtn state], self);
}

- (IBAction)recordShiftClicked:(id)sender
{
	if ([recordBtn state]) {
		session->record_enable_all();
	} else {
		session->record_disenable_all();
	}
}

- (void)updateRecord
{
	[recordBtn setState:route->record_enabled()];
}

- (NSString*)name
{
	return name;
}

- (void)setName:(NSString *)n
{
	[n retain];
	[name release];
	name = n;
	
	route->set_name([name cStringUsingEncoding:NSUTF8StringEncoding], self);
}

- (void)updateName
{
	[self setName:[NSString stringWithCString:route->name().c_str() encoding:NSUTF8StringEncoding]];
}

@end

void muteDisplayGlue (void* src, id sender)
{
	[sender performSelectorOnMainThread:@selector(updateMute) withObject:nil waitUntilDone:NO];
}

void soloDisplayGlue (void* src, id sender)
{
	[sender performSelectorOnMainThread:@selector(updateSolo) withObject:nil waitUntilDone:NO];
}

void recordDisplayGlue (void* src, id sender)
{
	[sender performSelectorOnMainThread:@selector(updateRecord) withObject:nil waitUntilDone:NO];
}

void nameDisplayGlue (void* src, id sender)
{
	[sender performSelectorOnMainThread:@selector(updateName) withObject:nil waitUntilDone:NO];
}
