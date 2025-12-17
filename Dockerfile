FROM emscripten/emsdk:4.0.17 AS build

RUN npm install -g esbuild

WORKDIR /build

COPY src /build/src/
COPY js /build/js/
COPY scripts /build/scripts/
COPY packages /build/packages/
COPY package.json /build/
COPY build.sh /build/

RUN --mount=type=cache,id=em-cache,target=/em_cache \
    --mount=type=cache,id=npm-cache,target=/root/.npm \
	EM_CACHE=/em_cache bash build.sh

FROM node:22-slim AS runtime

WORKDIR /app

RUN npm install -g serve

COPY ./example /app
COPY --from=build /build/dist /app/dist
COPY --from=build /build/dist/synthdefs /app/dist/synthdefs
COPY --from=build /build/dist/samples /app/dist/samples

EXPOSE 3000

CMD ["serve", "-l", "3000"]
