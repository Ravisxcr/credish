PYTHON ?= $(shell if [ -x .venv/bin/python ]; then printf '.venv/bin/python'; else printf 'python'; fi)
PIP ?= $(PYTHON) -m pip
BUILD ?= $(PYTHON) -m build
CIBUILDWHEEL ?= $(PYTHON) -m cibuildwheel
TWINE ?= $(PYTHON) -m twine
DIST_DIR ?= dist
CIBW_VERSION ?= 3.4.1
CIBW_DOCKER_IMAGE ?= python:3.12-bookworm
CIBW_BUILD ?= cp310-* cp311-* cp312-* cp313-* cp314-*
CIBW_SKIP ?= *-win32 *-musllinux*
CIBW_ARCHS_LINUX ?= x86_64

.PHONY: help bootstrap clean build build-wheels build-linux-wheels-docker setup-qemu check upload upload-test install-dev test

help:
	@printf '%s\n' \
		'Targets:' \
		'  make bootstrap    Install local packaging tools' \
		'  make clean        Remove build artifacts' \
		'  make build        Build sdist and wheel into dist/' \
		'  make build-wheels Build all configured manylinux wheels; run setup-qemu first for foreign archs' \
		'  make build-linux-wheels-docker Build Linux wheels via Docker into dist/ using mounted output' \
		'  make setup-qemu   Register Docker QEMU emulators for foreign arch wheels' \
		'  make check        Validate dist/* with twine' \
		'  make upload       Upload dist/* to PyPI with twine' \
		'  make upload-test  Upload dist/* to TestPyPI with twine' \
		'  make install-dev  Install this project editable with dev deps' \
		'  make test         Run tests'

bootstrap:
	$(PIP) install --upgrade build twine cibuildwheel

clean:
	rm -rf build/ $(DIST_DIR)/ *.egg-info credish.egg-info
	find credish -maxdepth 1 -name '*.so' -delete
	find credish src tests docs -name '__pycache__' -type d -prune -exec rm -rf {} +

build: clean
	$(BUILD)

build-wheels: clean
	$(CIBUILDWHEEL) --output-dir $(DIST_DIR)

build-linux-wheels-docker: clean
	mkdir -p $(DIST_DIR)
	docker run --rm \
		-v /var/run/docker.sock:/var/run/docker.sock \
		-v "$(CURDIR)":/project \
		-v "$(CURDIR)/$(DIST_DIR)":/output \
		-w /project \
		-e CIBW_BUILD="$(CIBW_BUILD)" \
		-e CIBW_SKIP="$(CIBW_SKIP)" \
		-e CIBW_ARCHS_LINUX="$(CIBW_ARCHS_LINUX)" \
		$(CIBW_DOCKER_IMAGE) sh -lc 'apt-get update && apt-get install -y --no-install-recommends docker.io && python -m pip install cibuildwheel==$(CIBW_VERSION) && python -m cibuildwheel /project --platform linux --output-dir /output'

setup-qemu:
	docker run --privileged --rm tonistiigi/binfmt --install arm64

check:
	$(TWINE) check $(DIST_DIR)/*

upload: check
	$(TWINE) upload $(DIST_DIR)/*

upload-test: check
	$(TWINE) upload --repository testpypi --skip-existing --verbose $(DIST_DIR)/*

install-dev:
	$(PIP) install -e '.[dev]'

test:
	$(PYTHON) -m pytest tests
