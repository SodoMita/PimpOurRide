#ifndef CD_VEHICLE_BODY_H
#define CD_VEHICLE_BODY_H
#include "chrome.h"

/* One coherent spline shell is shared by exterior paint, driver glazing and
 * the systems wire cage. Seats are separate detailed geometry, not EV boxes. */
void vehicle_body_render(Server *s);
void vehicle_body_render_glass(Server *s);
void vehicle_body_outline(float r,float g,float b);
void vehicle_seats_render(void);
#endif
