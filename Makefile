PYTHON ?= $(shell if [ -x .venv/bin/python ]; then printf '.venv/bin/python'; else printf 'python'; fi)
PIP ?= $(PYTHON) -m pip
BUILD ?= $(PYTHON) -m build
AUDITWHEEL ?= $(PYTHON) -m auditwheel
TWINE ?= $(PYTHON) -m twine
DIST_DIR ?= dist
WHEELHOUSE ?= wheelhouse

.PHONY: help bootstrap clean build repair-wheel check upload upload-test install-dev

help:
	@printf '%s\n' \
		'Targets:' \
		'  make bootstrap    Install local packaging tools' \
		'  make clean        Remove build artifacts' \
		'  make build        Build sdist and wheel into dist/' \
		'  make repair-wheel Repair Linux wheel tags for PyPI upload' \
		'  make check        Validate dist/* with twine' \
		'  make upload       Upload dist/* to PyPI with twine' \
		'  make upload-test  Upload dist/* to TestPyPI with twine' \
		'  make install-dev  Install this project editable with dev deps'

bootstrap:
	$(PIP) install --upgrade build twine auditwheel patchelf

clean:
	rm -rf build/ $(DIST_DIR)/ $(WHEELHOUSE)/ *.egg-info credish.egg-info
	find credish -maxdepth 1 -name '*.so' -delete
	find credish src tests benchmarks docs -name '__pycache__' -type d -prune -exec rm -rf {} +

build: clean
	$(BUILD)

repair-wheel: build
	PATH="$(abspath $(dir $(PYTHON))):$$PATH" $(AUDITWHEEL) repair --wheel-dir $(WHEELHOUSE) $(DIST_DIR)/*-linux_x86_64.whl
	rm -f $(DIST_DIR)/*-linux_x86_64.whl
	cp $(WHEELHOUSE)/*.whl $(DIST_DIR)/

check: repair-wheel
	$(TWINE) check $(DIST_DIR)/*

upload: check
	$(TWINE) upload $(DIST_DIR)/*

upload-test: check
	$(TWINE) upload --repository testpypi --skip-existing --verbose $(DIST_DIR)/*

install-dev:
	$(PIP) install -e '.[dev]'
