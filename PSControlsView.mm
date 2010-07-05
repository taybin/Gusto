//
//  PSControlsView.mm
//  Gusto
//
//  Created by taybin on 3/9/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#import "PSControlsView.h"


@implementation PSControlsView

- (id)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        // Initialization code here.
    }
    return self;
}

- (BOOL)isFlipped
{
	return YES;
}

- (void)drawRect:(NSRect)rect {
	[[NSColor colorWithDeviceRed:0.925 green:0.941 blue:0.969 alpha:1.0] set];  //standard light blue used in Mail.app and countless others.
	NSRectFill(rect);
}

@end
