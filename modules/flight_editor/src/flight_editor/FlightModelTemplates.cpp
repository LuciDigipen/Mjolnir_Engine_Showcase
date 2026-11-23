/**
 * @file FlightModelTemplates.cpp
 * @author Jackey Cen
 * @brief The built-in flight models, as L2-only JSON with no nodes.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include <flight_editor/FlightModelTemplates.h>

namespace mir
{
    namespace
    {
        constexpr char kPoweredTemplate[] = R"json({"version":1,"mass":1100.0,"inertia":{"diagonal":[1300.0,1800.0,2700.0],"products":[0.0,0.0,0.0]},"centerOfMass":[0.55,0.0,0.05],"reference":{"area":16.0,"span":11.0,"chord":1.5},"camera":{"eye":[1.95,-0.3,-0.55],"chaseDistance":14.0,"chaseHeight":3.5},"throttleRate":0.5,"stallWarningMarginDeg":4.0,"actuators":{"elevator":{"maxDeflectionDeg":25.0,"rateDegPerSec":60.000004,"delay":0.05},"aileron":{"maxDeflectionDeg":20.0,"rateDegPerSec":80.0,"delay":0.05},"rudder":{"maxDeflectionDeg":25.0,"rateDegPerSec":60.000004,"delay":0.05},"flaps":{"detentsDeg":[0.0,10.0,20.0,30.000002],"rateDegPerSec":5.0},"trim":{"stepDeg":0.5,"maxDeg":10.0}},"engines":[{"position":[4.05,0.0,0.05],"direction":[1.0,0.0,0.0],"idleRpm":700.0,"maxRpm":2700.0,"rpmTau":1.2,"staticThrust":[[0.0,0.0],[0.26,0.0],[1.0,3500.0]],"speedFactor":[[0.0,1.0],[35.0,0.75],[70.0,0.45]]}],"gear":[{"name":"nose","mount":[3.25,0.0,0.65],"restLength":0.98,"stiffness":25000.0,"damping":3000.0,"rollingFriction":0.03,"sideFriction":0.8,"brake":0.0,"maxSteerDeg":30.000002},{"name":"left main","mount":[-0.15,-1.7,0.65],"restLength":0.98,"stiffness":40000.0,"damping":5000.0,"rollingFriction":0.03,"sideFriction":0.9,"brake":0.6,"maxSteerDeg":0.0},{"name":"right main","mount":[-0.15,1.7,0.65],"restLength":0.98,"stiffness":40000.0,"damping":5000.0,"rollingFriction":0.03,"sideFriction":0.9,"brake":0.6,"maxSteerDeg":0.0}],"aero":{"L2":{"CL0":0.25,"CLalpha":5.2,"CD0":0.028,"k":0.05,"Cm0":0.05,"Cmalpha":-1.0,"Cmq":-14.0,"Cmde":1.3,"CYbeta":-0.5,"CYdr":-0.15,"Clbeta":-0.08,"Clp":-0.45,"Clda":0.17,"Cnbeta":0.09,"Cnr":-0.12,"Cndr":0.08,"Cnda":-0.012,"minAirspeed":15.0,"stall":{"positiveDeg":15.000001,"negativeDeg":-12.0,"blendDeg":3.0,"controlScale":0.5,"stabilityScale":0.6,"postStall":[[-30.000002,-0.8,0.65,0.3],[-15.000001,-0.7,0.22,0.12],[-12.0,-0.8,0.12,0.06],[0.0,0.25,0.03,0.05],[15.000001,1.5,0.15,-0.05],[18.0,1.2,0.25,-0.15],[22.0,1.05,0.38,-0.22],[30.000002,0.9,0.6,-0.35]]},"flaps":[[0.0,0.0,0.0,0.0,0.0],[10.0,0.25,0.012,-0.03,-1.0],[20.0,0.5,0.03,-0.06,-2.0],[30.000002,0.7,0.06,-0.09,-3.0]]}}})json";
        constexpr char kGliderTemplate[] = R"json({"version":1,"mass":450.0,"inertia":{"diagonal":[2500.0,700.0,3000.0],"products":[0.0,0.0,0.0]},"centerOfMass":[0.55,0.0,0.05],"reference":{"area":12.0,"span":15.0,"chord":0.8},"camera":{"eye":[1.95,-0.3,-0.55],"chaseDistance":14.0,"chaseHeight":3.5},"throttleRate":0.5,"stallWarningMarginDeg":4.0,"actuators":{"elevator":{"maxDeflectionDeg":25.0,"rateDegPerSec":60.000004,"delay":0.05},"aileron":{"maxDeflectionDeg":20.0,"rateDegPerSec":80.0,"delay":0.05},"rudder":{"maxDeflectionDeg":25.0,"rateDegPerSec":60.000004,"delay":0.05},"flaps":{"detentsDeg":[0.0,10.0,20.0,30.000002],"rateDegPerSec":5.0},"trim":{"stepDeg":0.5,"maxDeg":10.0}},"engines":[],"gear":[{"name":"nose","mount":[3.25,0.0,0.65],"restLength":0.98,"stiffness":10000.0,"damping":1500.0,"rollingFriction":0.03,"sideFriction":0.8,"brake":0.0,"maxSteerDeg":30.000002},{"name":"left main","mount":[-0.15,-1.7,0.65],"restLength":0.98,"stiffness":15000.0,"damping":2200.0,"rollingFriction":0.03,"sideFriction":0.9,"brake":0.6,"maxSteerDeg":0.0},{"name":"right main","mount":[-0.15,1.7,0.65],"restLength":0.98,"stiffness":15000.0,"damping":2200.0,"rollingFriction":0.03,"sideFriction":0.9,"brake":0.6,"maxSteerDeg":0.0}],"aero":{"L2":{"CL0":0.35,"CLalpha":5.6,"CD0":0.012,"k":0.022,"Cm0":0.04,"Cmalpha":-0.8,"Cmq":-12.0,"Cmde":1.1,"CYbeta":-0.4,"CYdr":-0.12,"Clbeta":-0.07,"Clp":-0.55,"Clda":0.15,"Cnbeta":0.06,"Cnr":-0.1,"Cndr":0.06,"Cnda":-0.015,"minAirspeed":10.0,"stall":{"positiveDeg":15.000001,"negativeDeg":-12.0,"blendDeg":3.0,"controlScale":0.5,"stabilityScale":0.6,"postStall":[[-30.000002,-0.8,0.65,0.3],[-15.000001,-0.7,0.22,0.12],[-12.0,-0.8,0.12,0.06],[0.0,0.25,0.03,0.05],[15.000001,1.5,0.15,-0.05],[18.0,1.2,0.25,-0.15],[22.0,1.05,0.38,-0.22],[30.000002,0.9,0.6,-0.35]]},"flaps":[[0.0,0.0,0.0,0.0,0.0],[10.0,0.25,0.012,-0.03,-1.0],[20.0,0.5,0.03,-0.06,-2.0],[30.000002,0.7,0.06,-0.09,-3.0]]}}})json";
    }

    std::string_view FlightModelTemplateJson(FlightModelTemplate kind)
    {
        switch (kind)
        {
        case FlightModelTemplate::Powered:
            return kPoweredTemplate;
        case FlightModelTemplate::Glider:
            return kGliderTemplate;
        }
        return {};
    }

    std::string_view FlightModelTemplateName(FlightModelTemplate kind)
    {
        switch (kind)
        {
        case FlightModelTemplate::Powered:
            return "Powered";
        case FlightModelTemplate::Glider:
            return "Glider";
        }
        return {};
    }
} // namespace mir
