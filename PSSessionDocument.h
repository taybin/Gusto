//
//  PSSessionDocument.h
//  Gusto
//
//  Created by Taybin Rutkin on 2/20/07.
//  Copyright Penguin Sounds 2007 . All rights reserved.
//


#import <Cocoa/Cocoa.h>

namespace ARDOUR {
	class Session;
}

@interface PSSessionDocument : NSDocument
{
	ARDOUR::Session *sess;
}


- (IBAction)addTrack:(id)sender;
- (void)tracksAdded;

- (void)saveSession;

@end
