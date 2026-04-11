"""
Sample Python application for RAG demo.
When errors reference functions/classes here, the CodeIndexer should find them.
"""

import logging
from datetime import datetime

logger = logging.getLogger(__name__)


class AuthMiddleware:
    """Handles authentication for incoming requests."""

    def handle_request(self, request):
        token = request.headers.get('Authorization')
        if not token:
            raise ValueError("Missing authorization token")

        if self.is_token_expired(token):
            raise ValueError("Token expired")

        return self.validate_token(token)

    def validate_token(self, token):
        """Validate JWT token against the auth server."""
        # In production, this would call the auth service
        if len(token) < 10:
            raise ValueError("Invalid token format")
        return {"user_id": 123, "role": "admin"}

    def is_token_expired(self, token):
        """Check if the token has expired."""
        return False  # Simplified for demo


class UserService:
    """Manages user operations."""

    def __init__(self, db_connection):
        self.db = db_connection

    def get_user(self, user_id):
        """Fetch user by ID from database."""
        if user_id <= 0:
            raise ValueError(f"Invalid user ID: {user_id}")
        return self.db.query(f"SELECT * FROM users WHERE id = {user_id}")

    def process_payment(self, user_id, amount):
        """Process a payment for the given user."""
        user = self.get_user(user_id)
        if not user:
            raise RuntimeError(f"User not found: {user_id}")

        if amount <= 0:
            raise ValueError(f"Invalid payment amount: {amount}")

        logger.info(f"Processing payment of ${amount} for user {user_id}")
        # Payment processing logic...
        return {"status": "success", "transaction_id": "txn_abc123"}


class OrderProcessor:
    """Handles order lifecycle management."""

    def create_order(self, items, user_id):
        """Create a new order from the given items."""
        if not items:
            raise ValueError("Cannot create order with empty items")
        return {"order_id": "ord_123", "status": "pending"}

    def cancel_order(self, order_id):
        """Cancel an existing order."""
        logger.warning(f"Cancelling order {order_id}")
        return {"order_id": order_id, "status": "cancelled"}


def schedule_backup():
    """Run scheduled database backup."""
    timestamp = datetime.now().isoformat()
    logger.info(f"Starting backup at {timestamp}")
    # Backup logic...
    raise IOError("Scheduled backup failed: disk full")
