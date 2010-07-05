//
//  PSMixerWindowController.h
//  Gusto
//
//  Created by Taybin on 3/14/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#import <Cocoa/Cocoa.h>

#import "PSMixerView.h"

namespace ARDOUR {
	class Session;
}

@interface PSMixerWindowController : NSWindowController {
	ARDOUR::Session *sess;
	NSMutableArray *routeArray;
	
	IBOutlet PSMixerView *mixerView;
}

- (void)connectToSession;
- (void)initialRouteListDisplay;
- (void)routesAdded:(ARDOUR::Session::RouteList&)rl;

@end
