//
//  PSFastMeter.mm
//  Gusto
//
//  Created by Taybin on 3/17/07.
//  Copyright 2007 Penguin Sounds. All rights reserved.
//

#include <algorithm>

#import "PSFastMeter.h"

@implementation PSFastMeter

- (id)initWithFrame:(NSRect)frame
{
    self = [super initWithFrame:frame];
    if (self) {
    }
    return self;
}

- (void)drawRect:(NSRect)rect {
	// set initial black
	[[NSColor blackColor] set];
	NSRectFill(rect);
	
	// fake log calculation copied from log_meter.h
	// actual calculation:
	// log_meter(0.0f) =
	//  def = (0.0f + 20.0f) * 2.5f + 50f
	//  return def / 115.0f
	int knee = (int)floor((float)(level * rect.size.height) * 100.0f / 115.0f);

	NSRect greenRect = NSMakeRect(rect.origin.x, rect.origin.y, rect.size.width, knee);
	[[NSColor greenColor] set];
	NSRectFill(greenRect);
	
	NSRect redRect = NSMakeRect(rect.origin.x, greenRect.origin.y + knee, rect.size.width, level * rect.size.height);
	[[NSColor redColor] set];
	NSRectFill(redRect);
}

- (void)setLevel:(float)l
{
	level = l;
	
	if (level > peak) {
		peak = level;
		holdState = holdCount;
	}
	
	if (holdState > 0) {
		if (--holdState == 0) {
			peak = level;
		}
	}
	
	[self setNeedsDisplay:YES];
}

- (void)clear
{
	level = 0;
	peak = 0;
	holdState = 0;
	[self setNeedsDisplay:YES];
}

- (float) level
{
	return level;
}

- (float) peak
{
	return peak;
}

- (unsigned long) holdCount
{
	return holdCount;
}

- (void)setHoldCount:(long)count
{
	holdCount = count;
}

@end
