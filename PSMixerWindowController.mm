//
//  PSMixerWindowController.mm
//  Gusto
//
//  Created by Taybin on 3/14/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#include <ardour/session.h>
#include <ardour/route.h>

#import "PSMixerWindowController.h"
#import "PSMixerStrip.h"

#include "objcsigcglue.h"

@implementation PSMixerWindowController

- (id)initWithSession:(ARDOUR::Session*)session
{
	self = [super initWithWindowNibName:@"MixerWindow"];
	if (self) {
		sess = session;
		routeArray = [[NSMutableArray alloc] initWithCapacity:1];
	}
	return self;
}

- (void)dealloc
{
	[routeArray release];
	[super dealloc];
}

- (void)windowDidLoad
{
	[self connectToSession];
}

- (void)connectToSession
{
	sess->RouteAdded.connect(sigc::bind(sigc::ptr_fun(objcsigcglue<ARDOUR::Session::RouteList&>), self, @selector(routesAdded:)));
	
	[self initialRouteListDisplay];
}

struct EditorOrderRouteSorter {
    bool operator() (boost::shared_ptr<ARDOUR::Route> a, boost::shared_ptr<ARDOUR::Route> b) {
		/* use of ">" forces the correct sort order */
		return a->order_key ("editor") < b->order_key ("editor");
    }
};

- (void)initialRouteListDisplay
{
	boost::shared_ptr<ARDOUR::Session::RouteList> routes = sess->get_routes();
	
	ARDOUR::Session::RouteList r (*routes);
		
	EditorOrderRouteSorter sorter;
	r.sort (sorter);
		
	[self routesAdded:r];
}

- (void)routesAdded:(ARDOUR::Session::RouteList&)rl
{
	using namespace ARDOUR;
	for (Session::RouteList::iterator x = rl.begin(); x != rl.end(); ++x) {
		boost::shared_ptr<Route> route = (*x);
		
		if (route->hidden()) {
			continue;
		}
		
		NSPoint point;
		point.x = [routeArray count] * 92;
		point.y = 0.0;
		
		PSMixerStrip *mixerStrip = [[PSMixerStrip alloc] initWithRoute:route withSession:sess];
		[mixerView addSubview:[mixerStrip stripView] positioned:NSWindowAbove relativeTo:[routeArray lastObject]];
		[[mixerStrip stripView] setFrameOrigin:point];
		[routeArray addObject:mixerStrip];
	}
}

#pragma mark -
#pragma mark Delegates

- (NSSize)windowWillResize:(NSWindow *)sender toSize:(NSSize)proposedFrameSize
{
	proposedFrameSize.height = 750;  //constrain to mixerstrip height
	return proposedFrameSize;
}

@end
