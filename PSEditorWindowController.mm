//
//  PSEditorWindowController.mm
//  Gusto
//
//  Created by taybin on 3/12/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#include <ardour/route.h>
#include <ardour/session.h>

#import "PSEditorTrack.h"
#import "PSEditorWindowController.h"

#include "objcsigcglue.h"

@implementation PSEditorWindowController

- (id)initWithSession:(ARDOUR::Session*)session
{
	self = [super initWithWindowNibName:@"EditorWindow"];
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

- (IBAction)play:(id)sender
{
	sess->request_transport_speed (1.0f);
}

- (IBAction)record:(id)sender
{
	
}

- (IBAction)stop:(id)sender
{
	sess->request_stop();
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
		point.x = 0;
		point.y = [routeArray count] * 49;
		
		PSEditorTrack *editorTrack = [[PSEditorTrack alloc] initWithRoute:route withSession:sess];
		[controlsView addSubview:[editorTrack trackControl] positioned:NSWindowAbove relativeTo:[routeArray lastObject]];
		[[editorTrack trackControl] setFrameOrigin:point];
		[routeArray addObject:editorTrack];
	}
}

@end
