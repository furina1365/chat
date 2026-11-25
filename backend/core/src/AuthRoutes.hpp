#pragma once

#include "Api.hpp"

class HttpRouter;

namespace chat
{
void RegisterAuthRoutes(HttpRouter &router, auth::Auth &auth, SubmitTask submit,
                        const AuthHttpConfig &config);
} // namespace chat
