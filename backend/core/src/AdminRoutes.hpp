#pragma once
#include "Api.hpp"

namespace chat
{
void RegisterAdminRoutes(HttpRouter &router, auth::Auth &auth, SubmitTask submit, AuthHttpConfig config);
}
