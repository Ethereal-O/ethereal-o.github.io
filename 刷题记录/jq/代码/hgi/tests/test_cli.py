from hgi.cli import parser


def test_enum_cli_options_parse_on_python_311():
    args = parser().parse_args(
        ["--commit-mode", "assist", "--interrupt-mode", "auto", "--writes", "deny"]
    )

    assert args.commit_mode == "assist"
    assert args.interrupt_mode == "auto"
    assert args.writes == "deny"
