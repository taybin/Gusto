//
//  PSFastMeter.h
//  Gusto
//
//  Created by Taybin on 3/17/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#import <Cocoa/Cocoa.h>


@interface PSFastMeter : NSView {
	int widthRequest;
	int heightRequest;
	unsigned long holdCount;
	unsigned long holdState;
	float level;
	float peak;
}

- (id)initWithFrame:(NSRect)frame;

- (void)setLevel:(float)l;
- (void)clear;

- (float) level;
- (float) peak;

- (unsigned long) holdCount;
- (void)setHoldCount:(long)count;

@end
